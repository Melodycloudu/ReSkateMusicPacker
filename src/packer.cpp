// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// ReSkateMusicPacker: a folder of songs -> a mod that ADDS them to the game's music.
// Nothing shipped is replaced; every song gets its own assets and audio.
//
// The library behind ReSkateMusicPacker (CLI) and the music maker app: scan() reads what the
// songs are called, pack() builds the mod. Any file ffmpeg can read is a song (ffmpeg and
// ffprobe must be on PATH).
//
// What one song is, in the game's data (all in bam_levelroot/bam_coregameassets):
//   <slug>_MG   MusicGraphAsset, cloned from a shipped song: name, NameHash, artist/title
//               and its wave import change; every instance gets a fresh GUID.
//   <slug>_NWA  NewWaveAsset, cloned from that song's wave: its two chunk ids and sizes.
//   two TOC chunks: a prefetch chunk (22-byte header + the stream's seek table) and the
//               stream (EA blocks: one H header, one D block per Opus packet, one E end).
//   an entry in Audio/Music/Playlist/DGO_MUS_Playlist's Assets: the audio engine only
//   plays songs listed there; registration alone makes a song listable, not playable.
// The bundle manifest is the region's first file, stored raw in cas like the game's own:
// the game crashes on a bundle whose manifest is inline in the TOC.
#include "packer.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "miniz.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Core/Platform/path_case.h"
#include "Engine/Core/Platform/path_text.h"
#include "platform.h"
#include "sha.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace ebx = fb::ebx;
using dingosdk::Json;


namespace {
constexpr char toc_path[] = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr char bundle_name[] = "win32/levels/game/bam_levelroot/bam_coregameassets";
constexpr char playlist_asset[] = "audio/music/playlist/dgo_mus_playlist";
// The template song and the wave its music segment plays. Belly Dancer: one music
// segment, one silent lead-in, 48 kHz stereo Opus like every shipped song.
constexpr char template_song[] = "audio/music/assets/turkish/turkish_upm_belly_dancer_mg";
constexpr char template_wave[] = "audio/music/assets/turkish/turkish_upm_belly_dancer_chapwr13a_1_nwa";
constexpr char asset_folder[] = "Audio/Music/Assets/ReSkate/";
// Where the shipped bundle's manifest lives (defaultinstallpackage); the mod's archive goes beside it.
constexpr std::uint32_t install_chunk = 2016875820;
constexpr char cas_relative[] = "Win32/configurations/layout/defaultinstallpackage/cas_01.cas";
// Every shipped song's audio packet is one 20 ms Opus frame.
constexpr std::uint32_t packet_samples = 960;


struct Song {
    fs::path file;
    std::string artist, title, slug, playlist;
};

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.string());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}
void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Cannot write " + path.string());
}
fb::Sha1 sha1_of(std::span<const std::byte> bytes) {
    music::Sha1 hash;
    hash.update(bytes);
    const auto digest = hash.finish();
    fb::Sha1 result;
    std::memcpy(result.bytes.data(), digest.data(), 20);
    return result;
}
template <std::size_t N> std::string to_hex(const std::array<std::byte, N>& bytes) {
    std::string hex(N * 2, '\0');
    for (std::size_t i = 0; i < N; ++i)
        std::snprintf(hex.data() + i * 2, 3, "%02x", static_cast<unsigned char>(bytes[i]));
    return hex;
}
std::string to_hex(const fb::Sha1& sha) { return to_hex(sha.bytes); }
fb::Sha1 sha1_of_file(const fs::path& path) {
    const auto digest = music::sha1_file(path);
    fb::Sha1 result;
    std::memcpy(result.bytes.data(), digest.data(), 20);
    return result;
}
// A random (version 4) GUID, laid out as CoCreateGuid's in memory.
fb::Guid new_guid() {
    static std::mutex mutex;
    static std::random_device device;
    static std::mt19937_64 random{(static_cast<std::uint64_t>(device()) << 32) ^ device()};
    fb::Guid result;
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < 16; i += 8) {
            const auto value = random();
            std::memcpy(result.bytes.data() + i, &value, 8);
        }
    }
    result.bytes[7] = static_cast<std::byte>((std::to_integer<unsigned>(result.bytes[7]) & 0x0F) | 0x40);
    result.bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(result.bytes[8]) & 0x3F) | 0x80);
    return result;
}

// A cache entry goes in whole or not at all: it is written under a name of its own and renamed
// into place. The destination is therefore only ever created whole, and never truncated while
// another thread reads it. When the rename loses a race the entry is simply dropped: two writers
// of one path are encoding the same source bytes, so what they would have written is identical.
void write_file_atomic(const fs::path& path, std::string_view text) {
    auto temporary = path;
    temporary += ".tmp." + to_hex(new_guid().bytes);
    write_file(temporary, std::as_bytes(std::span(text.data(), text.size())));
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) fs::remove(temporary, ec);
}

using platform::narrow;
using platform::widen;
using dingosdk::path_utf8;
using dingosdk::resolve_case;

// A program and its arguments, run without a shell (platform::run_process), so a file name with
// quotes, spaces or $ reaches ffmpeg exactly as it is.
using Args = std::vector<std::string>;
void run(const Args& args) {
    if (platform::run_process(args) != 0) throw std::runtime_error("Command failed: " + platform::command_line(args));
}
std::string run_capture(const Args& args) {
    std::string output;
    if (platform::run_process(args, &output) != 0) throw std::runtime_error("Command failed: " + platform::command_line(args));
    return output;
}

// Packs a folder into a .zip with miniz, so the Thunderstore export needs no external `tar`.
void zip_write(const fs::path& archive, const fs::path& folder) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) throw std::runtime_error("Could not start the package archive");
    struct Guard { mz_zip_archive* zip; ~Guard() { mz_zip_writer_end(zip); } } guard{&zip};
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        const auto name = fs::relative(entry.path(), folder).generic_string();
        const auto bytes = read_file(entry.path());
        if (!mz_zip_writer_add_mem_ex(&zip, name.c_str(), bytes.data(), bytes.size(), nullptr, 0,
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION), 0, 0))
            throw std::runtime_error("Could not add " + name + " to the package");
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) || !buffer)
        throw std::runtime_error("Could not build the package archive");
    write_file(archive, std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer), size));
}

// ---- metadata ----------------------------------------------------------------------------------

// The bundle stores asset names lowercased.
std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Artist and title as the music menu shows them: the song's id is "artist - title".
bool usable(const std::string& text) {
    return !text.empty() && text.size() <= 255 &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    const auto start = text.find_first_not_of(" \t");
    return start == std::string::npos ? std::string{} : text.substr(start);
}

// Where scan and pack keep ffprobe's and ffmpeg's output.
fs::path scratch_folder() {
#ifdef _WIN32
    const auto scratch = fs::temp_directory_path() / "ReSkateMusicPacker";
#else
    // /tmp is shared by every user, so another account could own the folder or plant symlinks at
    // the predictable names ffmpeg overwrites (-y); the user's own cache folder is private.
    const auto base = platform::cache_directory();
    const auto scratch = (base.empty() ? fs::temp_directory_path() : base) / "ReSkateMusicPacker" / "scratch";
#endif
    std::error_code ec;   // several threads may create it at once; that is not a failure
    fs::create_directories(scratch, ec);
    std::error_code missing;
    if (!fs::is_directory(scratch, missing))
        throw std::runtime_error("Cannot create " + scratch.string());
    return scratch;
}

// Where transcoded Opus files are cached so rebuilding doesn't re-encode unchanged songs.
fs::path cache_folder() {
    const auto base = platform::cache_directory();
    const auto folder = base.empty() ? scratch_folder() / "cache" : base / "ReSkateMusicPacker" / "cache";
    std::error_code ec;
    fs::create_directories(folder, ec);
    return folder;
}

int attached_picture(const Json& root) {
    if (root.contains("streams") && root.at("streams").is_array())
        for (const auto& stream : root.at("streams"))
            if (stream.contains("disposition") && stream.at("disposition").value("attached_pic", 0) == 1)
                return stream.at("index").get<int>();
    return -1;
}
constexpr char artwork_filter[] =
    "scale=512:512:force_original_aspect_ratio=decrease,pad=512:512:(ow-iw)/2:(oh-ih)/2:color=0x242035,setsar=1";

// ---- audio -------------------------------------------------------------------------------------

struct Opus {
    std::uint16_t preSkip{};
    std::uint64_t samples{};               // playable samples: last granule minus pre-skip
    std::vector<std::vector<std::byte>> packets;
};

struct EncodedOpus {
    Opus opus;
    bool cached = false;
};

// Samples in one Opus packet, from its TOC byte (RFC 6716 section 3.1).
std::uint32_t samples_in(const std::vector<std::byte>& packet) {
    if (packet.empty()) throw std::runtime_error("Empty Opus packet");
    const auto toc = static_cast<unsigned>(packet[0]);
    const auto config = toc >> 3;
    std::uint32_t frame;
    if (config < 12) frame = std::array<std::uint32_t, 4>{480, 960, 1920, 2880}[config & 3];
    else if (config < 16) frame = (config & 1) ? 960 : 480;
    else frame = std::array<std::uint32_t, 4>{120, 240, 480, 960}[config & 3];
    std::uint32_t frames = 1;
    if ((toc & 3) == 1 || (toc & 3) == 2) frames = 2;
    else if ((toc & 3) == 3) {
        if (packet.size() < 2) throw std::runtime_error("Truncated Opus packet");
        frames = static_cast<unsigned>(packet[1]) & 0x3F;
    }
    return frame * frames;
}

// The packets of an Ogg Opus file: OpusHead first, then OpusTags, then audio.
Opus read_ogg_opus(std::span<const std::byte> ogg) {
    Opus result;
    std::vector<std::vector<std::byte>> packets;
    std::vector<std::byte> partial;
    std::int64_t granule{};
    std::size_t at = 0;
    while (at + 27 <= ogg.size()) {
        if (std::memcmp(ogg.data() + at, "OggS", 4) != 0) throw std::runtime_error("Not an Ogg stream");
        std::int64_t pageGranule;
        std::memcpy(&pageGranule, ogg.data() + at + 6, 8);
        const auto segments = static_cast<std::size_t>(ogg[at + 26]);
        if (at + 27 + segments > ogg.size()) throw std::runtime_error("Truncated Ogg page");
        const auto* table = ogg.data() + at + 27;
        auto data = at + 27 + segments;
        for (std::size_t s = 0; s < segments; ++s) {
            const auto length = static_cast<std::size_t>(table[s]);
            if (data + length > ogg.size()) throw std::runtime_error("Truncated Ogg page");
            partial.insert(partial.end(), ogg.begin() + data, ogg.begin() + data + length);
            data += length;
            if (length < 255) { packets.push_back(std::move(partial)); partial.clear(); }
        }
        if (pageGranule >= 0) granule = pageGranule;
        at = data;
    }
    if (packets.size() < 3 || packets[0].size() < 19 || std::memcmp(packets[0].data(), "OpusHead", 8) != 0)
        throw std::runtime_error("Not an Ogg Opus stream");
    const auto channels = static_cast<unsigned>(packets[0][9]);
    if (channels != 2) throw std::runtime_error("Expected stereo Opus");
    std::memcpy(&result.preSkip, packets[0].data() + 10, 2);
    if (granule <= result.preSkip) throw std::runtime_error("Opus stream has no audio");
    result.samples = static_cast<std::uint64_t>(granule) - result.preSkip;
    result.packets.assign(std::make_move_iterator(packets.begin() + 2), std::make_move_iterator(packets.end()));
    return result;
}

// The game's own tracks sit at about -15 LUFS (measured from the shipped UPM and licensed streams),
// so normalise to that. Two passes: measure the input, then apply a linear gain so each song lands on
// the target instead of the single-pass dynamic loudnorm's drift. The cache tag changes with the
// target so an older -16 encode is not reused.
constexpr char loudness_filter[] = "I=-15.0:TP=-1.5:LRA=11";
constexpr char loudness_cache_tag[] = "_norm15";

std::string loudness_af(const fs::path& file, const std::string& fileKey) {
    const auto base = std::string("loudnorm=") + loudness_filter;
    // The measurement depends on the source audio and the target, never on the bitrate, so it is
    // cached on its own: rebuilding at another bitrate re-encodes without decoding every song a
    // second time just to measure it. Only a real measurement is stored, and the prefix is checked
    // on read, so a damaged or stale entry is measured again rather than used.
    const auto cached = cache_folder() / widen(fileKey + "_measure_" + loudness_cache_tag + ".json");
    std::error_code ignored;
    if (fs::is_regular_file(cached, ignored)) {
        std::ifstream in(cached, std::ios::binary);
        const std::string stored{std::istreambuf_iterator<char>(in), {}};
        // The prefix only proves the entry is for this target; the tail is where a truncated or
        // half-written one gives itself away, and that tail is exactly what ffmpeg parses.
        if (stored.starts_with(base) && stored.ends_with(":linear=true") &&
            stored.find(":measured_I=") != std::string::npos && stored.find(":measured_TP=") != std::string::npos &&
            stored.find(":measured_thresh=") != std::string::npos && stored.find(":offset=") != std::string::npos)
            return stored;
    }
    try {
        const auto text = run_capture({"ffmpeg", "-hide_banner", "-nostats", "-i", path_utf8(file), "-vn", "-map_metadata", "-1",
                                       "-ac", "2", "-ar", "48000", "-af", base + ":print_format=json", "-f", "null", "-"});
        const auto start = text.rfind('{'), end = text.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end < start) return base;
        const auto root = Json::parse(text.substr(start, end - start + 1));
        const auto value = [&](const char* key) {
            return root.contains(key) && root.at(key).is_string() ? root.at(key).string() : std::string{};
        };
        const auto i = value("input_i"), lra = value("input_lra"), tp = value("input_tp");
        const auto thresh = value("input_thresh"), offset = value("target_offset");
        if (i.empty() || tp.empty()) return base;
        auto measured = base + ":measured_I=" + i + ":measured_LRA=" + lra + ":measured_TP=" + tp +
                        ":measured_thresh=" + thresh + ":offset=" + offset + ":linear=true";
        try { write_file_atomic(cached, measured); } catch (const std::exception&) {}
        return measured;
    } catch (const std::exception&) {
        return base; // measurement failed: fall back to the dynamic filter
    }
}

// A plain count from a string: digits only, whole string, no sign. strtoul reads a leading '-' as
// a sign and hands back ULONG_MAX, and a partial parse would take "8ffmpeg" as 8.
bool count_from(std::string_view text, int& out) {
    int value = 0;
    const auto* first = text.data();
    const auto [stop, error] = std::from_chars(first, first + text.size(), value);
    if (error != std::errc{} || stop != first + text.size() || value <= 0) return false;
    out = value;
    return true;
}

// The cap is not decoration: RESKATE_JOBS=4096 on a laptop is a fork bomb, not a setting.
std::size_t capped_slots(int requested) {
    return std::clamp<std::size_t>(static_cast<std::size_t>(requested), 1, 64);
}

// Half the logical CPUs: each ffmpeg already wants several threads of its own, so this keeps every
// core busy without the pile-on that makes all of them slower.
std::size_t automatic_slots() {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency() / 2);
}

EncodedOpus encode(const Song& song, int bitrate, bool normalize, const fs::path& scratch, std::size_t index) {
    const auto fileSha = to_hex(sha1_of_file(song.file));
    const auto tag = std::to_string(bitrate) + "k" + (normalize ? loudness_cache_tag : "");
    const auto cachedFile = cache_folder() / widen(fileSha + "_" + tag + ".ogg");

    std::error_code ec;
    if (fs::exists(cachedFile, ec)) {
        try {
            auto opus = read_ogg_opus(read_file(cachedFile));
            bool valid = !opus.packets.empty();
            for (const auto& packet : opus.packets) {
                if (samples_in(packet) != packet_samples) { valid = false; break; }
            }
            if (valid) return EncodedOpus{std::move(opus), true};
        } catch (...) {
            // Corrupt cached entry; fall through to re-encode
        }
        fs::remove(cachedFile, ec);
    }

    // The song index keeps two songs with identical bytes (the same file added under two names)
    // from handing `ffmpeg -y` the same output path. It has to be the index, not a pool slot:
    // a slot is reused, so the same two songs could still meet on one name.
    const auto ogg = scratch / widen(fileSha + "_" + std::to_string(index) + "_" + tag + ".tmp.ogg");
    Args command{"ffmpeg", "-y", "-v", "error", "-i", path_utf8(song.file), "-vn", "-map_metadata", "-1", "-ac", "2", "-ar", "48000"};
    if (normalize) command.insert(command.end(), {"-af", loudness_af(song.file, fileSha)});
    command.insert(command.end(), {"-c:a", "libopus", "-b:a", std::to_string(bitrate) + "k", "-vbr", "on", "-frame_duration", "20",
                                   "-application", "audio", "-f", "ogg", path_utf8(ogg)});
    run(command);
    auto opus = read_ogg_opus(read_file(ogg));
    for (const auto& packet : opus.packets)
        if (samples_in(packet) != packet_samples) throw std::runtime_error("ffmpeg produced a packet that is not 20 ms");

    // Rename only: copying onto the destination would truncate the entry another thread may be
    // reading. Losing this race costs nothing, since the same source bytes encode identically.
    fs::rename(ogg, cachedFile, ec);
    if (ec) fs::remove(ogg, ec);

    return EncodedOpus{std::move(opus), false};
}

void put_u32be(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>(value >> shift));
}
void put_block(std::vector<std::byte>& out, std::uint8_t id, std::span<const std::byte> payload) {
    const auto size = static_cast<std::uint32_t>(payload.size() + 4);
    if (size > 0xFFFFFF) throw std::runtime_error("Audio block too large");
    put_u32be(out, (static_cast<std::uint32_t>(id) << 24) | size);
    out.insert(out.end(), payload.begin(), payload.end());
}

// The 10-byte codec header: its count is the playable sample total, matching the sum of the
// D blocks' own sample counts (the shipped songs store exactly that).
std::array<std::byte, 10> codec_header(std::span<const std::byte, 10> templateHeader, std::uint64_t samples) {
    if (samples >= (1ull << 26)) throw std::runtime_error("Song too long");
    std::array<std::byte, 10> header;
    std::copy(templateHeader.begin(), templateHeader.end(), header.begin());
    std::uint32_t word{};
    for (int i = 4; i < 8; ++i) word = (word << 8) | static_cast<unsigned>(header[i]);
    word = static_cast<std::uint32_t>(samples << 6) | (word & 0x3F);
    for (int i = 0; i < 4; ++i) header[4 + i] = static_cast<std::byte>(word >> (24 - 8 * i));
    return header;
}

// The playable sample total: exactly what the D blocks carry. The first block leaves out the
// encoder's pre-skip, the last leaves out any padding past the stream's granule. This is what
// `stream_chunk` writes and what the shipped songs store in the codec header, so it must be
// computed from the packets, not from the Ogg granule (ffmpeg writes the input duration there,
// which can exceed the decoded packet sum).
std::uint64_t playable_samples(const Opus& opus) {
    std::uint64_t remaining = opus.samples, total = 0;
    for (std::size_t index = 0; index < opus.packets.size() && remaining; ++index) {
        std::uint64_t yields = packet_samples - (index == 0 ? std::min<std::uint32_t>(opus.preSkip, packet_samples) : 0);
        yields = std::min(yields, remaining);
        remaining -= yields;
        total += yields;
    }
    return total;
}

// H header, one D block per packet (u32 BE samples it yields + the packet), E end. Like the
// shipped songs, the first block's count already leaves out the encoder's pre-skip and the
// last block's leaves out the padding past the end.
std::vector<std::byte> stream_chunk(const Opus& opus, std::span<const std::byte, 10> header) {
    std::vector<std::byte> out;
    put_block(out, 'H', header);
    auto remaining = opus.samples;
    for (std::size_t index = 0; index < opus.packets.size() && remaining; ++index) {
        std::uint64_t yields = packet_samples - (index == 0 ? std::min<std::uint32_t>(opus.preSkip, packet_samples) : 0);
        yields = std::min(yields, remaining);
        remaining -= yields;
        std::vector<std::byte> payload;
        put_u32be(payload, static_cast<std::uint32_t>(yields));
        payload.insert(payload.end(), opus.packets[index].begin(), opus.packets[index].end());
        put_block(out, 'D', payload);
    }
    const std::byte end{0};
    put_block(out, 'E', std::span(&end, 1));
    return out;
}

// ---- Seek table ----------------------------------------------------------------------------------
// The prefetch chunk's body is the stream's seek table, which the game walks to start a song
// part way through: the prefetch parser (0x14137db30; version 1, table type 1 = byte 1's high
// nibble) hands it to 0x14137dec0. One entry per H and D block: {block size in bytes, 0,
// samples the block yields, 1 on the first D block only}. Each column is its own stream of runs
// ("n >= 0": n + 1 entries of one value, one delta) and literal lists ("n < 0": 1 - n entries,
// one delta each), read in entry order: for each entry, column 0, 1, 2, 3, each reading its next
// header when the last ran out. Checked against 92,553 entries of 12 shipped songs (notes:
// "Prefetch chunk body").
struct SeekEntry { std::int64_t column[4]; };

// Signed varint (0x14137f720): the sign is the last byte's low bit and stores ~v. |v| < 96 is one
// byte (|v| * 2 + sign); |v| < 6240 two (0xC000 + ((|v| - 96) << 1 | sign), big endian). The game
// also reads 3- to 5-byte forms; the table below never needs them.
void put_varint(std::vector<std::byte>& out, std::int64_t v) {
    const unsigned sign = v < 0 ? 1 : 0;
    const std::int64_t m = sign ? ~v : v;
    if (m < 96) { out.push_back(static_cast<std::byte>(m * 2 + sign)); return; }
    if (m - 96 >= 0x1800) throw std::runtime_error("Seek table value out of range");
    const auto u = static_cast<std::uint32_t>(0xC000 + (((m - 96) << 1) | sign));
    out.push_back(static_cast<std::byte>(u >> 8));
    out.push_back(static_cast<std::byte>(u & 0xFF));
}
std::int64_t get_varint(std::span<const std::byte> in, std::size_t& at) {
    if (at >= in.size()) throw std::runtime_error("Seek table ends early");
    const auto b = static_cast<unsigned>(in[at]);
    if (b < 0xC0) { ++at; return b & 1 ? ~std::int64_t(b >> 1) : std::int64_t(b >> 1); }
    if (b >= 0xF0 || at + 1 >= in.size()) throw std::runtime_error("Seek table varint form not written by this tool");
    const auto u = (b << 8) | static_cast<unsigned>(in[at + 1]);
    at += 2;
    const std::int64_t m = ((u >> 1) & 0x1FFF) + 96;
    return u & 1 ? ~m : m;
}

std::vector<SeekEntry> seek_entries(std::span<const std::byte> stream) {
    std::vector<SeekEntry> entries;
    for (std::size_t at = 0; at + 4 <= stream.size();) {
        const auto id = static_cast<char>(stream[at]);
        const auto size = (static_cast<std::uint32_t>(stream[at + 1]) << 16) | (static_cast<std::uint32_t>(stream[at + 2]) << 8) |
                          static_cast<std::uint32_t>(stream[at + 3]);
        if (id == 'E') break;
        if ((id != 'H' && id != 'D') || size < 4 || at + size > stream.size()) throw std::runtime_error("Unexpected stream block");
        std::int64_t samples = 0;
        if (id == 'D')
            for (int i = 4; i < 8; ++i) samples = (samples << 8) | static_cast<unsigned>(stream[at + i]);
        entries.push_back({{static_cast<std::int64_t>(size), 0, samples, entries.size() == 1 ? 1 : 0}});
        at += size;
    }
    return entries;
}

std::vector<std::byte> seek_table(const std::vector<SeekEntry>& entries) {
    // Each column as runs of equal values (all but the sizes, which vary per block) and literal
    // lists, at most 6000 entries each so every header fits the two-byte varint.
    struct Segment { bool run; std::vector<std::int64_t> values; };
    constexpr std::size_t most = 6000;
    std::array<std::vector<Segment>, 4> segments;
    for (int k = 0; k < 4; ++k) {
        auto& out = segments[k];
        for (std::size_t i = 0; i < entries.size();) {
            std::size_t j = i;
            while (j < entries.size() && entries[j].column[k] == entries[i].column[k] && j - i < most) ++j;
            if (k != 0 && j - i >= 2) {
                out.push_back({true, std::vector<std::int64_t>(j - i, entries[i].column[k])});
                i = j;
            } else {
                if (out.empty() || out.back().run || out.back().values.size() >= most) out.push_back({false, {}});
                out.back().values.push_back(entries[i++].column[k]);
            }
        }
        // A one-entry list would need header 0, which means a run: write it as a run of one.
        for (auto& segment : out) if (!segment.run && segment.values.size() == 1) segment.run = true;
    }
    std::vector<std::byte> out;
    struct Cursor { std::size_t segment{}, left{}, next{}; std::int64_t value{}; };
    std::array<Cursor, 4> cursors{};
    for (std::size_t e = 0; e < entries.size(); ++e)
        for (int k = 0; k < 4; ++k) {
            auto& c = cursors[k];
            if (c.left == 0) {
                const auto& s = segments[k][c.segment++];
                c.left = s.values.size();
                c.next = 0;
                if (s.run) {
                    put_varint(out, static_cast<std::int64_t>(s.values.size()) - 1);
                    put_varint(out, s.values[0] - c.value);
                    c.value = s.values[0];
                } else {
                    put_varint(out, 1 - static_cast<std::int64_t>(s.values.size()));
                }
            }
            const auto& s = segments[k][c.segment - 1];
            if (!s.run) {
                put_varint(out, s.values[c.next] - c.value);
                c.value = s.values[c.next];
            }
            ++c.next;
            --c.left;
        }
    return out;
}

// Walks a table as 0x14137dec0 does and checks it gives back every entry.
void check_seek_table(std::span<const std::byte> table, const std::vector<SeekEntry>& entries) {
    struct Column { std::int64_t value{}, left{}; bool run{}; };
    std::array<Column, 4> columns{};
    std::size_t at = 0;
    for (const auto& entry : entries)
        for (int k = 0; k < 4; ++k) {
            auto& c = columns[k];
            if (c.left <= 0) {
                const auto n = get_varint(table, at);
                c.run = n >= 0;
                c.left = c.run ? n + 1 : 1 - n;
                if (c.run) c.value += get_varint(table, at);
            }
            if (!c.run) c.value += get_varint(table, at);
            --c.left;
            if (c.value != entry.column[k]) throw std::runtime_error("Seek table does not read back");
        }
}

// The prefetch chunk: [01 10][u16 BE pre-skip][u32 BE 22, the table's offset][u32 0, no optional
// data][codec header][seek table], zero-padded to 4 bytes like the shipped chunks.
std::vector<std::byte> prefetch_chunk(std::span<const std::byte> templatePrefetch, const Opus& opus,
                                      std::span<const std::byte, 10> header, std::span<const std::byte> stream) {
    if (templatePrefetch.size() < 22 || static_cast<unsigned>(templatePrefetch[0]) != 1 ||
        static_cast<unsigned>(templatePrefetch[1]) != 0x10 || static_cast<unsigned>(templatePrefetch[7]) != 22)
        throw std::runtime_error("Template prefetch chunk has an unexpected layout");
    std::vector<std::byte> out(templatePrefetch.begin(), templatePrefetch.begin() + 12);
    out[2] = static_cast<std::byte>(opus.preSkip >> 8);
    out[3] = static_cast<std::byte>(opus.preSkip & 0xFF);
    out.insert(out.end(), header.begin(), header.end());
    const auto entries = seek_entries(stream);
    const auto table = seek_table(entries);
    check_seek_table(table, entries);
    out.insert(out.end(), table.begin(), table.end());
    while (out.size() % 4) out.push_back(std::byte{0});
    return out;
}

// ---- EBX edits ---------------------------------------------------------------------------------

ebx::FieldValue& field(ebx::Object& object, std::string_view name) {
    for (auto& f : object.fields) if (f.name == name) return f;
    throw std::runtime_error("No field " + std::string(name));
}
void set_number(ebx::FieldValue& f, std::uint64_t value) {
    if (std::holds_alternative<std::int64_t>(f.value.data)) f.value.data = static_cast<std::int64_t>(value);
    else f.value.data = value;
}
ebx::InstanceRecord& root_of(ebx::Document& document, std::string_view type) {
    for (auto& instance : document.instances)
        if (instance.object && instance.descriptor >= 0 && document.types[instance.descriptor].name == type) return instance;
    throw std::runtime_error("No " + std::string(type) + " instance");
}
// A fresh partition: new file GUID and instance GUIDs. Internal pointers are by index.
void renew(ebx::Document& document) {
    document.fileGuid = new_guid();
    for (auto& instance : document.instances) instance.instanceGuid = new_guid();
}
std::uint32_t name_hash(std::string_view name) {
    // djb2-xor of the name's last component, as the shipped assets carry it.
    std::uint32_t hash = 5381;
    for (const auto c : name.substr(name.rfind('/') + 1)) hash = (hash * 33) ^ static_cast<unsigned char>(c);
    return hash;
}

struct Built {
    std::string name;                      // lower-case bundle asset name
    std::vector<std::byte> payload;        // decoded EBX or resource
};

// ---- the wave's sound-bank resource ------------------------------------------------------------

// Every NewWaveAsset has a bundle resource of this type under the same name: an "SBle"
// sound-bank blob. Loading it registers the wave's instance GUID with the audio system
// (Skate.exe sub_141485D30 -> sub_14148BE50); a wave without one is looked up there anyway when
// it is instantiated (sub_1414841B0) and the game crashes on the not-found sentinel.
constexpr std::uint32_t wave_resource_type = 0xb2c465f6;

// What differs between two songs' blobs, by value. The blob is the template's with each of
// these replaced; a value found other than the expected number of times stops the build
// rather than leave a half-patched blob.
struct WaveFacts {
    fb::Guid instance, prefetchId, streamId;
    std::uint32_t prefetchSize{}, streamSize{};
    std::uint64_t samples{}; // SoundBank duration, matching the H header's playable sample total.
    std::string name;
};

std::uint32_t djb2(std::string_view text) {
    std::uint32_t hash = 5381;
    for (const auto c : text) hash = (hash * 33) ^ static_cast<unsigned char>(c);
    return hash;
}

void replace_all(std::vector<std::byte>& blob, std::span<const std::byte> from, std::span<const std::byte> to,
                 std::size_t expected, const char* what) {
    std::size_t found = 0;
    for (auto at = blob.begin(); (at = std::search(at, blob.end(), from.begin(), from.end())) != blob.end(); at += from.size()) {
        std::copy(to.begin(), to.end(), at);
        ++found;
    }
    if (found != expected)
        throw std::runtime_error(std::string("Template wave resource: ") + what + " found " + std::to_string(found) +
                                 " time(s), expected " + std::to_string(expected));
}
template<class T> std::array<std::byte, sizeof(T)> le(T value) {
    std::array<std::byte, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

std::vector<std::byte> wave_resource(std::vector<std::byte> blob, const WaveFacts& from, const WaveFacts& to) {
    // An id derived from the wave: djb2-xor of its instance GUID as text (CONFIRMED on the template).
    replace_all(blob, le(djb2(from.instance.string())), le(djb2(to.instance.string())), 7, "the wave id");
    // A second per-wave id whose source is not known; it only has to be unique, so it is
    // derived from the new wave's name. ponytail: unknown semantics, find its source if a song misbehaves.
    const auto unknown = [&] { std::uint32_t v; std::memcpy(&v, blob.data() + 216, 4); return v; }();
    replace_all(blob, le(unknown), le(djb2(to.name)), 2, "the second wave id");
    replace_all(blob, le(static_cast<float>(from.samples / 48000.0)), le(static_cast<float>(to.samples / 48000.0)), 1, "the duration");
    replace_all(blob, le(from.prefetchSize), le(to.prefetchSize), 1, "the prefetch size");
    replace_all(blob, le(from.streamSize - from.prefetchSize), le(to.streamSize - to.prefetchSize), 2, "the stream length");
    replace_all(blob, from.prefetchId.bytes, to.prefetchId.bytes, 1, "the prefetch chunk id");
    replace_all(blob, from.streamId.bytes, to.streamId.bytes, 1, "the stream chunk id");
    replace_all(blob, from.instance.bytes, to.instance.bytes, 1, "the wave GUID");
    return blob;
}

// What one song contributes to the mod. It is built apart from the playlist document and the
// chunk/asset vectors so that several songs can be here at once.
struct EncodedSong {
    std::vector<std::byte> prefetch, stream;
    fb::Guid prefetchId, streamId;            // made here, appended to `chunks` in song order
    fb::Guid songFileGuid, songInstanceGuid;  // the song's partition and its MusicGraphAsset
    std::vector<Built> assets;                // the wave's EBX, then the song's
    std::vector<Built> resources;             // the wave's sound-bank resource
    std::string detail;                       // what the progress report shows
    std::exception_ptr error;                 // read only after the pool has joined
};

// One song end to end: encode it, build its two chunks, and write the wave and song documents plus
// the wave's sound-bank resource. Every input is read-only and the only shared state reached is
// new_guid(), which is already locked, so several songs may do this at the same time.
EncodedSong encode_song(const Song& song, std::size_t index, int bitrate, bool normalize, const fs::path& scratch,
                        std::span<const std::byte> waveBytes, std::span<const std::byte> songBytes,
                        const fb::Guid& templateFile, std::span<const std::byte, 10> templateHeader,
                        std::span<const std::byte> templatePrefetch, const WaveFacts& templateFacts,
                        std::span<const std::byte> templateBlob) {
    EncodedSong result;
    const auto encoded = encode(song, bitrate, normalize, scratch, index);
    const auto& opus = encoded.opus;
    const auto samples = playable_samples(opus);
    const auto header = codec_header(templateHeader, samples);
    result.stream = stream_chunk(opus, header);
    result.prefetch = prefetch_chunk(templatePrefetch, opus, header, result.stream);
    char detail[96];
    std::snprintf(detail, sizeof detail, "%.1f s, %zu packets, %zu KB%s", opus.samples / 48000.0, opus.packets.size(),
                  result.stream.size() / 1024, encoded.cached ? " (cached)" : "");
    result.detail = detail;

    // The wave: fresh partition, its own two chunks. Parsed anew per song: a copied
    // Document shares its instances' objects with the original.
    auto wave = ebx::read_document(waveBytes);
    renew(wave);
    auto& waveRoot = *root_of(wave, "NewWaveAsset").object;
    const auto waveName = std::string(asset_folder) + song.slug + "_NWA";
    field(waveRoot, "Name").value.data = waveName;
    auto& waveChunks = std::get<ebx::Value::Array>(field(waveRoot, "Chunks").value.data);
    if (waveChunks.size() != 2) throw std::runtime_error("Template wave does not have two chunks");
    for (std::size_t k = 0; k < 2; ++k) {
        const auto& bytes = k == 0 ? result.prefetch : result.stream;
        auto& entry = *std::get<std::shared_ptr<ebx::Object>>(waveChunks[k].data);
        const auto id = new_guid();
        field(entry, "ChunkId").value.data = id;
        set_number(field(entry, "ChunkSize"), bytes.size());
        if (k == 0) result.prefetchId = id; else result.streamId = id;
    }

    // The song: fresh partition, its music segment's wave import pointing at the new wave.
    auto graph = ebx::read_document(songBytes);
    renew(graph);
    bool rewired = false;
    for (auto& import : graph.imports)
        if (import.fileGuid == templateFile) {
            import = {wave.fileGuid, root_of(wave, "NewWaveAsset").instanceGuid};
            rewired = true;
        }
    if (!rewired) throw std::runtime_error("Template song does not import its wave");
    const auto songName = std::string(asset_folder) + song.slug + "_MG";
    auto& graphRoot = *root_of(graph, "MusicGraphAsset").object;
    field(graphRoot, "Name").value.data = songName;
    set_number(field(graphRoot, "NameHash"), name_hash(songName));
    auto& metadata = *root_of(graph, "DingoMusicMetadata").object;
    field(metadata, "ArtistName").value.data = song.artist;
    field(metadata, "TrackName").value.data = song.title;
    // No station tag: the template's would list the song under its station (Turkish).
    // The playlist comes from reskate-music.json instead; the licensed songs carry no
    // tags either. The orphaned MusicAssetTag instance is harmless.
    std::get<ebx::Value::Array>(field(graphRoot, "TagRefs").value.data).clear();
    std::get<ebx::Value::Array>(field(graphRoot, "Tags").value.data).clear();
    result.songFileGuid = graph.fileGuid;
    result.songInstanceGuid = root_of(graph, "MusicGraphAsset").instanceGuid;

    WaveFacts facts;
    facts.instance = root_of(wave, "NewWaveAsset").instanceGuid;
    facts.prefetchId = result.prefetchId;
    facts.streamId = result.streamId;
    facts.prefetchSize = static_cast<std::uint32_t>(result.prefetch.size());
    facts.streamSize = static_cast<std::uint32_t>(result.stream.size());
    facts.samples = samples;
    facts.name = waveName;
    result.assets.push_back({lowered(waveName), ebx::write_document(wave)});
    result.assets.push_back({lowered(songName), ebx::write_document(graph)});
    result.resources.push_back({lowered(waveName),
                                wave_resource(std::vector<std::byte>(templateBlob.begin(), templateBlob.end()),
                                              templateFacts, facts)});
    return result;
}

// ---- the mod -----------------------------------------------------------------------------------

std::string skate_sha256(const fs::path& game) {
    return music::hex_digest(music::sha256_file(resolve_case(game / "Skate.exe")));
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const auto c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else out += c;
    }
    return out + "\"";
}


// Builds the whole mod into `out`, an empty folder.
music::PackResult build(const music::PackOptions& options, const fs::path& out, const std::vector<music::SongInfo>& files,
                 const music::ProgressFn& progress, const std::atomic<bool>* cancel) {
    const auto report = [&](std::size_t song, const char* stage, std::string detail = {}) {
        if (progress) progress({song, files.size(), stage, std::move(detail)});
    };
    const auto scratch = scratch_folder();

    dingosdk::vfs::GameData data(options.game);
    const auto toc = data.read_toc(toc_path);
    auto bundle = data.read_bundle(toc, bundle_name);
    if (!bundle) throw std::runtime_error("The game has no bam_coregameassets bundle");
    const fb::TocBundle* shipped{};
    for (const auto& b : toc.bundles) if (b.name == bundle_name) shipped = &b;
    if (fb::read_bundle_region(shipped->region).files.front().location.installChunk != install_chunk)
        throw std::runtime_error("bam_coregameassets is not where this tool expects; is this the supported game build?");

    const auto ebx_of = [&](const char* name, std::size_t& index) {
        const auto* asset = bundle->find(fb::AssetKind::ebx, name, &index);
        if (!asset) throw std::runtime_error(std::string("The game has no ") + name);
        return data.read(*bundle->payload(fb::AssetKind::ebx, index));
    };
    std::size_t songIndex{}, waveIndex{}, playlistIndex{};
    const auto songBytes = ebx_of(template_song, songIndex);
    const auto waveBytes = ebx_of(template_wave, waveIndex);
    auto playlist = ebx::read_document(ebx_of(playlist_asset, playlistIndex));
    const auto templateWave = ebx::read_document(waveBytes);

    // The template wave's facts: its GUID, its two chunks (prefetch first), its length.
    WaveFacts templateFacts;
    std::vector<std::byte> templatePrefetch;
    {
        auto wave = ebx::read_document(waveBytes);
        auto& root = root_of(wave, "NewWaveAsset");
        templateFacts.instance = root.instanceGuid;
        const auto& list = std::get<ebx::Value::Array>(field(*root.object, "Chunks").value.data);
        if (list.size() != 2) throw std::runtime_error("Template wave does not have two chunks");
        const auto chunk_at = [&](std::size_t k) -> std::pair<fb::Guid, std::uint32_t> {
            auto& entry = *std::get<std::shared_ptr<ebx::Object>>(list[k].data);
            const auto& size = field(entry, "ChunkSize").value.data;
            return {std::get<fb::Guid>(field(entry, "ChunkId").value.data),
                    static_cast<std::uint32_t>(std::holds_alternative<std::uint64_t>(size) ? std::get<std::uint64_t>(size)
                                                                                          : std::get<std::int64_t>(size))};
        };
        std::tie(templateFacts.prefetchId, templateFacts.prefetchSize) = chunk_at(0);
        std::tie(templateFacts.streamId, templateFacts.streamSize) = chunk_at(1);
        for (const auto& chunk : toc.chunks)
            if (chunk.guid == templateFacts.prefetchId) templatePrefetch = data.read({chunk.location, chunk.offset, chunk.size});
        if (templatePrefetch.size() != templateFacts.prefetchSize) throw std::runtime_error("Template prefetch chunk not found");
    }
    std::array<std::byte, 10> templateHeader;
    std::copy_n(templatePrefetch.begin() + 12, 10, templateHeader.begin());
    {
        std::uint32_t word{};
        for (int i = 4; i < 8; ++i) word = (word << 8) | static_cast<unsigned>(templateHeader[i]);
        templateFacts.samples = word >> 6;
    }
    std::size_t templateResourceIndex{};
    const auto* templateResource = bundle->find(fb::AssetKind::resource, template_wave, &templateResourceIndex);
    if (!templateResource || templateResource->resourceType != wave_resource_type)
        throw std::runtime_error("The template wave has no sound-bank resource");
    const auto templateBlob = data.read(*bundle->payload(fb::AssetKind::resource, templateResourceIndex));

    std::vector<Built> assets, resources;
    std::vector<fb::TocChunk> chunks;
    std::vector<std::byte> cas;   // the manifest goes in front once everything else is placed
    std::vector<std::pair<std::size_t, std::size_t>> chunkData;   // offset/size in `chunkBytes` per TOC chunk
    std::vector<std::byte> chunkBytes;
    std::set<std::string> ids, slugs;
    std::vector<std::string> order;
    // Check every song before encoding any: a bad name should not cost minutes of encoding first.
    std::vector<Song> songs;
    for (const auto& info : files) {
        if (!usable(info.artist) || !usable(info.title))
            throw std::runtime_error(narrow(info.file.filename().wstring()) + ": artist or title is empty, too long, or has control characters");
        const auto& pName = info.playlist.empty() ? options.playlist : info.playlist;
        if (!usable(pName))
            throw std::runtime_error(narrow(info.file.filename().wstring()) + ": playlist name is empty, too long, or has control characters");
        // Fold smart quotes before the name becomes an asset path or a song id, so a
        // mis-tagged title cannot produce a byte-identical lowercased asset name.
        const auto artist = music::sanitize_text(info.artist);
        const auto title = music::sanitize_text(info.title);
        Song song{info.file, artist, title, music::slug_of(artist, title), pName};
        const auto id = song.artist + " - " + song.title;
        if (!ids.insert(id).second) throw std::runtime_error("Two songs are both \"" + id + "\"");
        order.push_back(id);
        // The bundle stores asset names lowercased, so uniqueness is case-insensitive too.
        song.slug = music::unique_slug(song.slug, slugs);
        songs.push_back(std::move(song));
    }
    // The encode loop is two blocking ffmpeg processes per song and almost nothing else, so songs
    // go through in parallel. encode_song() keeps to per-song state; the two shared things, the
    // playlist document and the chunk/asset vectors, are only touched below, in song order, so the
    // mod comes out exactly as the one-song-at-a-time loop left it.
    std::vector<EncodedSong> encodedSongs(songs.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex reportLock; // ProgressFn is called from workers, so it must never run re-entrantly
    const auto reportSong = [&](std::size_t index, const char* stage, std::string detail = {}) {
        std::lock_guard lock(reportLock);
        report(index, stage, std::move(detail));
    };
    auto worker = [&] {
        for (std::size_t index = next++; index < songs.size(); index = next++) {
            if (cancel && *cancel) return;   // the check below turns this into music::Cancelled
            // A song that failed stops the rest, as it did when this loop was one song at a time:
            // there is no point spending another hour of ffmpeg on a mod that cannot be written.
            // This is a flag rather than a scan of encodedSongs: the other workers are still
            // assigning into that vector, and reading their error pointers while they do is a race.
            if (failed.load(std::memory_order_relaxed)) return;
            try {
                reportSong(index, "encoding");
                auto song = encode_song(songs[index], index, options.bitrate, options.normalize, scratch,
                                        waveBytes, songBytes, templateWave.fileGuid, templateHeader,
                                        templatePrefetch, templateFacts, templateBlob);
                reportSong(index, "encoded", song.detail);
                encodedSongs[index] = std::move(song);
            } catch (...) {
                encodedSongs[index].error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
            }
        }
    };
    std::vector<std::thread> pool;
    try {
        for (std::size_t slot = 0, slots = std::min(songs.size(), encode_slots_for(options)); slot < slots; ++slot)
            pool.emplace_back(worker);
    } catch (...) {
        for (auto& thread : pool) thread.join(); // a joinable thread destroyed here is std::terminate
        throw;
    }
    for (auto& thread : pool) thread.join();
    // A cancel is what the user asked for, so it outranks an ffmpeg error that happened to land
    // in the same window. Otherwise the first real failure wins, in song order, so what the user
    // is told does not depend on which thread happened to finish first.
    if (cancel && *cancel) throw music::Cancelled();
    for (const auto& song : encodedSongs)
        if (song.error) std::rethrow_exception(song.error);

    // Every chunk size is known now, so the archive is sized once instead of doubling its way up
    // (which copies the whole library each time and briefly holds one and a half copies of it).
    std::size_t archiveBytes = 0;
    for (const auto& song : encodedSongs) archiveBytes += song.prefetch.size() + song.stream.size();
    chunkBytes.reserve(archiveBytes);

    for (std::size_t index = 0; index < songs.size(); ++index) {
        auto& song = encodedSongs[index];
        playlist.imports.push_back({song.songFileGuid, song.songInstanceGuid});
        std::get<ebx::Value::Array>(field(*root_of(playlist, "MusicPlaylistAsset").object, "Assets").value.data)
            .push_back(ebx::Value{ebx::PointerReference{ebx::PointerKind::external,
                                                        static_cast<std::int32_t>(playlist.imports.size() - 1)}});
        for (std::size_t k = 0; k < 2; ++k) {
            const auto& bytes = k == 0 ? song.prefetch : song.stream;
            fb::TocChunk chunk;
            chunk.guid = k == 0 ? song.prefetchId : song.streamId;
            chunks.push_back(chunk);
            chunkData.push_back({chunkBytes.size(), bytes.size()});
            chunkBytes.insert(chunkBytes.end(), bytes.begin(), bytes.end());
        }
        // The bytes now live in chunkBytes. Holding them here as well would keep two copies of
        // the whole library resident, where the one-song-at-a-time loop kept one. Swap rather than
        // shrink_to_fit, whose release is a request the standard does not oblige anyone to honour:
        // giving the capacity back is the entire point of these two lines.
        std::vector<std::byte>().swap(song.prefetch);
        std::vector<std::byte>().swap(song.stream);
        for (auto& asset : song.assets) assets.push_back(std::move(asset));
        for (auto& resource : song.resources) resources.push_back(std::move(resource));
    }

    report(files.size(), "building");
    // Bundle: the shipped manifest plus the new assets and wave resources, the master playlist
    // rewritten. Region files run parallel to the manifest: ebx, then resources, then chunks.
    auto manifest = bundle->manifest;
    const auto ebxCount = manifest.ebx.size(), resourceCount = manifest.resources.size();
    const auto& shippedFiles = bundle->payloads;
    std::vector<fb::BundleFileInfo> ebxFiles(shippedFiles.begin(), shippedFiles.begin() + ebxCount);
    std::vector<fb::BundleFileInfo> resourceFiles(shippedFiles.begin() + ebxCount, shippedFiles.begin() + ebxCount + resourceCount);
    std::vector<fb::BundleFileInfo> chunkFiles(shippedFiles.begin() + ebxCount + resourceCount, shippedFiles.end());
    struct Placed { std::vector<fb::BundleFileInfo>* list; std::size_t index; std::vector<std::byte> encoded; };
    std::vector<Placed> placed;
    const auto playlistBytes = ebx::write_document(playlist);
    manifest.ebx[playlistIndex].sha1 = sha1_of(playlistBytes);
    manifest.ebx[playlistIndex].originalSize = playlistBytes.size();
    placed.push_back({&ebxFiles, playlistIndex, fb::encode_cas(playlistBytes, {options.game})});
    for (const auto& asset : assets) {
        auto entry = manifest.ebx[songIndex];
        entry.name = asset.name;
        entry.sha1 = sha1_of(asset.payload);
        entry.originalSize = asset.payload.size();
        manifest.ebx.push_back(entry);
        ebxFiles.emplace_back();
        placed.push_back({&ebxFiles, ebxFiles.size() - 1, fb::encode_cas(asset.payload, {options.game})});
    }
    for (const auto& resource : resources) {
        auto entry = *templateResource;
        entry.name = resource.name;
        const auto id = new_guid();
        std::memcpy(&entry.resourceId, id.bytes.data(), sizeof(entry.resourceId));
        entry.sha1 = sha1_of(resource.payload);
        entry.originalSize = resource.payload.size();
        manifest.resources.push_back(entry);
        resourceFiles.emplace_back();
        placed.push_back({&resourceFiles, resourceFiles.size() - 1, fb::encode_cas(resource.payload, {options.game})});
    }
    const auto manifestBytes = fb::write_binary_bundle(manifest);
    cas = manifestBytes;
    for (auto& entry : placed) {
        (*entry.list)[entry.index] = {{true, install_chunk, 1}, static_cast<std::uint32_t>(cas.size()),
                                      static_cast<std::uint32_t>(entry.encoded.size())};
        cas.insert(cas.end(), entry.encoded.begin(), entry.encoded.end());
    }
    std::vector<fb::BundleFileInfo> files_{{{true, install_chunk, 1}, 0, static_cast<std::uint32_t>(manifestBytes.size())}};
    for (const auto* list : {&ebxFiles, &resourceFiles, &chunkFiles}) files_.insert(files_.end(), list->begin(), list->end());
    if (files_.size() != 1 + manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size())
        throw std::runtime_error("Placement count does not match the manifest");

    // Audio chunks: stored uncompressed (Opus does not compress), referenced from the TOC.
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        const auto encoded = fb::encode_cas(std::span(chunkBytes).subspan(chunkData[k].first, chunkData[k].second),
                                            {options.game, fb::CasCompression::raw});
        chunks[k].location = {true, install_chunk, 1};
        chunks[k].offset = static_cast<std::uint32_t>(cas.size());
        chunks[k].size = static_cast<std::uint32_t>(encoded.size());
        cas.insert(cas.end(), encoded.begin(), encoded.end());
    }
    if (cas.size() > 0xFFFFFFFFull) throw std::runtime_error("Too much audio for one archive");

    fb::TocBundle patched{bundle_name, fb::write_bundle_region(files_), shipped->loadFlag};
    const auto tocBytes = fb::write_patch_toc(std::span(&patched, 1), chunks);
    fb::verify_toc(fb::read_toc(tocBytes));

    report(files.size(), "writing");
    write_file(out / cas_relative, cas);
    write_file(out / toc_path, tocBytes);
    fs::copy_file(resolve_case(options.game / "Data" / "layout.toc"), out / L"layout.toc", fs::copy_options::overwrite_existing);
    const auto text = [](const std::string& s) { return std::as_bytes(std::span(s.data(), s.size())); };
    write_file(out / L".reskate-studio-patch",
               text("ReSkate Studio native Patch v1\nskate_sha256=" + skate_sha256(options.game) + "\n"));
    write_file(out / L"manifest.json",
               text("{\n  \"name\": " + json_string(options.name) + ",\n  \"version_number\": \"1.0.0\",\n"
                    "  \"description\": \"Adds " + std::to_string(files.size()) +
                    " song(s) to the game's music.\",\n  \"dependencies\": []\n}\n"));
    // The playlist ReSkate's music menu shows: the songs by their ids ("artist - title"),
    // in file order, grouped into playlists.
    {
        std::vector<std::string> playlistOrder;
        std::map<std::string, std::vector<std::string>> playlistSongs;
        for (std::size_t i = 0; i < songs.size(); ++i) {
            const auto& pName = songs[i].playlist;
            if (!playlistSongs.contains(pName)) playlistOrder.push_back(pName);
            playlistSongs[pName].push_back(order[i]);
        }
        auto metadata = Json::object();
        metadata["schema"] = 1;
        auto playlists = Json::array();
        auto covers = Json::object();
        std::map<fs::path, std::string> images;
        const auto image = [&](const fs::path& source) -> std::string {
            if (cancel && *cancel) throw music::Cancelled();
            if (source.empty()) return {};
            const auto absolute = fs::absolute(source);
            if (const auto found = images.find(absolute); found != images.end()) return found->second;
            if (!fs::is_regular_file(absolute)) throw std::runtime_error("Artwork file is missing: " + narrow(absolute.wstring()));
            const auto relative = "artwork/cover-" + std::to_string(images.size()) + ".png";
            const auto target = out / widen(relative);
            fs::create_directories(target.parent_path());
            write_file(target, music::image_artwork_png(absolute));
            if (cancel && *cancel) throw music::Cancelled();
            images.emplace(absolute, relative);
            return relative;
        };
        // Track artwork is resolved first so a playlist can borrow its first available cover.
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (cancel && *cancel) throw music::Cancelled();
            const auto source = files[i].artwork.empty() ? music::embedded_artwork(files[i].file) : files[i].artwork;
            if (!source.empty()) covers[order[i]] = image(source);
        }
        for (std::size_t pi = 0; pi < playlistOrder.size(); ++pi) {
            const auto& pName = playlistOrder[pi];
            auto entry = Json::object();
            entry["name"] = pName;
            entry["songs"] = Json::array();
            const auto& sList = playlistSongs[pName];
            for (const auto& id : sList) entry["songs"].push_back(id);
            if (const auto art = options.playlist_artwork.find(pName); art != options.playlist_artwork.end() && !art->second.empty())
                entry["artwork"] = image(art->second);
            else if (options.generated_playlist_artwork.contains(pName)) {
                if (cancel && *cancel) throw music::Cancelled();
                const auto relative = "artwork/playlist-" + std::to_string(pi) + ".png";
                write_file(out / widen(relative), music::playlist_artwork_png(pName));
                entry["artwork"] = relative;
            } else {
                for (const auto& id : sList)
                    if (covers.contains(id)) { entry["artwork"] = covers.at(id).string(); break; }
            }
            playlists.push_back(std::move(entry));
        }
        metadata["playlists"] = std::move(playlists);
        if (!covers.empty()) metadata["song_artwork"] = std::move(covers);
        write_file(out / L"reskate-music.json", text(metadata.dump(2) + "\n"));
    }
    write_file(out / L"reskate-build.json", text("{\n  \"schema\": 1,\n  \"tool\": \"ReSkateMusicPacker\",\n  \"version\": \"0.1.0\"\n}\n"));
    // What the mod was made from, so it can be reopened and changed (load_project).
    {
        auto project = Json::object();
        project["schema"] = 1;
        project["name"] = options.name;
        project["playlist"] = options.playlist;
        project["bitrate"] = options.bitrate;
        project["normalize"] = options.normalize;
        auto playlistArt = Json::object();
        for (const auto& [name, path] : options.playlist_artwork)
            if (!path.empty()) playlistArt[name] = narrow(fs::absolute(path).wstring());
        project["playlist_artwork"] = std::move(playlistArt);
        project["generated_playlist_artwork"] = Json::array();
        for (const auto& name : options.generated_playlist_artwork) project["generated_playlist_artwork"].push_back(name);
        auto list = Json::array();
        for (const auto& song : files) {
            auto entry = Json::object();
            entry["file"] = narrow(fs::absolute(song.file).wstring());
            entry["artist"] = song.artist;
            entry["title"] = song.title;
            if (!song.playlist.empty()) entry["playlist"] = song.playlist;
            if (!song.artwork.empty()) entry["artwork"] = narrow(fs::absolute(song.artwork).wstring());
            list.push_back(std::move(entry));
        }
        project["songs"] = std::move(list);
        write_file(out / L"reskate-music-project.json", text(project.dump(2) + "\n"));
    }
    return {files.size(), chunkBytes.size() / 1024, out};
}
} // namespace

namespace music {

bool usable_name(const std::string& text) { return usable(text); }

namespace {
// Smart quote / apostrophe sequences folded onto ASCII. The trailing rows are the
// double-encoded ("â€™") forms that mis-tagged files produce for the same characters.
struct QuoteFold { std::string_view from; char to; };
constexpr QuoteFold quote_folds[]{
    {"\xe2\x80\x98", '\''}, {"\xe2\x80\x99", '\''}, {"\xe2\x80\x9a", '\''}, {"\xe2\x80\xb2", '\''},
    {"\xe2\x80\x9c", '"'},  {"\xe2\x80\x9d", '"'},  {"\xe2\x80\x9e", '"'},
    {"\xc3\xa2\xe2\x82\xac\xe2\x84\xa2", '\''}, {"\xc3\xa2\xe2\x82\xac\xcb\x9c", '\''},
    {"\xc3\xa2\xe2\x82\xac\xe2\x80\x9a", '\''}, {"\xc3\xa2\xe2\x82\xac\xe2\x80\xb2", '\''},
    {"\xc3\xa2\xe2\x82\xac\xc5\x93", '"'}, {"\xc3\xa2\xe2\x82\xac\xc2\x9d", '"'},
    {"\xc3\xa2\xe2\x82\xac\xe2\x80\x9e", '"'},
};
std::string lower_ascii(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
} // namespace

std::string sanitize_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        bool folded = false;
        for (const auto& fold : quote_folds)
            if (text.compare(i, fold.from.size(), fold.from.data(), fold.from.size()) == 0) {
                out += fold.to;
                i += fold.from.size();
                folded = true;
                break;
            }
        if (!folded) out += text[i++];
    }
    return out;
}

std::string slug_of(const std::string& artist, const std::string& title) {
    std::string slug;
    for (const auto c : sanitize_text(artist) + "_" + sanitize_text(title)) {
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128) slug += c;
        else if (!slug.empty() && slug.back() != '_') slug += '_';
    }
    while (!slug.empty() && slug.back() == '_') slug.pop_back();
    if (slug.size() > 64) slug.resize(64);
    if (slug.empty()) slug = "Song";
    return slug;
}

std::string unique_slug(const std::string& base, std::set<std::string>& used) {
    std::string candidate = base;
    for (int n = 2; !used.insert(lower_ascii(candidate)).second; ++n) candidate = base + "_" + std::to_string(n);
    return candidate;
}

std::vector<std::byte> image_artwork_png(const fs::path& image) {
    // Named afresh per call, like every other scratch file: `ffmpeg -y` truncates its output, and
    // this one is reachable both from pack() and from the GUI's cover preview.
    const auto temporary = scratch_folder() / widen("image-preview_" + to_hex(new_guid().bytes) + ".png");
    std::error_code ignored;
    fs::remove(temporary, ignored);
    try {
        run({"ffmpeg", "-y", "-v", "error", "-i", path_utf8(image), "-an", "-vf", artwork_filter,
             "-frames:v", "1", "-update", "1", path_utf8(temporary)});
        auto bytes = read_file(temporary);
        fs::remove(temporary, ignored);
        return bytes;
    } catch (...) { fs::remove(temporary, ignored); throw; }
}

fs::path embedded_artwork(const fs::path& track) {
    fs::path temporary;
    try {
        const auto key = to_hex(sha1_of_file(track)) + "_cover_v2_fit";
        const auto cached = cache_folder() / widen(key + ".png");
        if (fs::is_regular_file(cached) && fs::file_size(cached) > 33) {
            const auto bytes = read_file(cached);
            if (bytes.size() > 33 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return cached;
        }
        const auto index = attached_picture(Json::parse(run_capture(
            {"ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=index:stream_disposition=attached_pic",
             "-of", "json", path_utf8(track)})));
        if (index < 0) return {};
        temporary = cache_folder() / widen(key + ".tmp.png");
        run({"ffmpeg", "-y", "-v", "error", "-i", path_utf8(track), "-map", "0:" + std::to_string(index), "-an", "-vf",
             artwork_filter, "-frames:v", "1", "-update", "1", path_utf8(temporary)});
        std::error_code ignored;
        fs::remove(cached, ignored);
        fs::rename(temporary, cached);
        return cached;
    } catch (const std::exception&) {
        std::error_code ignored;
        if (!temporary.empty()) fs::remove(temporary, ignored);
        return {}; // Missing or damaged optional artwork must not prevent audio packing.
    }
}

std::vector<SongInfo> scan(std::span<const fs::path> files) {
    std::vector<SongInfo> songs;
    for (const auto& file : files) {
        SongInfo song{file};
        try {
            const auto root = Json::parse(run_capture(
                {"ffprobe", "-v", "error", "-show_entries",
                 "format=duration:format_tags=artist,title:stream=index:stream_disposition=attached_pic", "-of", "json",
                 path_utf8(file)}));
            song.has_embedded_artwork = attached_picture(root) >= 0;
            const auto& format = root.at("format");
            if (format.contains("duration") && format.at("duration").is_string()) song.seconds = std::atof(format.at("duration").string().c_str());
            if (format.contains("tags")) {
                const auto& tags = format.at("tags");
                for (const char* key : {"artist", "ARTIST", "Artist"})
                    if (tags.contains(key) && tags.at(key).is_string()) { song.artist = trim(tags.at(key).string()); break; }
                for (const char* key : {"title", "TITLE", "Title"})
                    if (tags.contains(key) && tags.at(key).is_string()) { song.title = trim(tags.at(key).string()); break; }
            }
        } catch (const std::exception&) {
            song.problems.push_back("ffprobe could not read it");
        }
        if (song.artist.empty()) song.problems.push_back("no artist tag");
        if (song.title.empty()) song.problems.push_back("no title tag");
        if (song.artist.empty() || song.title.empty()) {
            const auto stem = narrow(file.stem().wstring());
            const auto dash = stem.find(" - ");
            if (song.artist.empty()) song.artist = dash == std::string::npos ? "Unknown Artist" : trim(stem.substr(0, dash));
            if (song.title.empty()) song.title = trim(dash == std::string::npos ? stem : stem.substr(dash + 3));
        }
        if (!usable(song.artist) || !usable(song.title))
            song.problems.push_back("artist or title is empty, too long, or has control characters");
        if (song.seconds * 48000 >= (1 << 26)) song.problems.push_back("too long (the codec header stores 26 bits of samples: 23 minutes at most)");
        songs.push_back(std::move(song));
    }
    return songs;
}

std::size_t encode_slots_for(const PackOptions& options) {
    // options.jobs is the explicit request and wins; RESKATE_JOBS is the power-user/CI override;
    // otherwise half the logical CPUs, since each ffmpeg already wants several threads of its own.
    if (options.jobs > 0) return capped_slots(options.jobs);
    int asked = 0;
    if (const auto set = platform::environment("RESKATE_JOBS"); !set.empty() && count_from(set, asked))
        return capped_slots(asked);
    return automatic_slots();
}

PackResult pack(const PackOptions& options, const std::vector<SongInfo>& songs, const ProgressFn& progress,
                const std::atomic<bool>* cancel) {
    if (songs.empty()) throw std::runtime_error("No songs to pack");
    if (!usable(options.playlist)) throw std::runtime_error("The playlist name is empty, too long, or has control characters");
    // Build beside the output and move it into place at the end, so a failure or a cancel
    // leaves no half-written mod.
    const auto& out = options.output;
    const auto partial = out.parent_path() / (out.filename().wstring() + L".partial");
    std::error_code ignored;
    fs::remove_all(partial, ignored);
    try {
        auto result = build(options, partial, songs, progress, cancel);
        result.output = out;
        if (cancel && *cancel) throw Cancelled();
        if (fs::exists(out)) {
            // ponytail: an existing output folder is overwritten file by file, as before, and may end up
            // part new and part old if this copy fails; build into a fresh folder to rule that out.
            fs::copy(partial, out, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::remove_all(partial);
        } else {
            fs::create_directories(out.parent_path());
            fs::rename(partial, out);
        }
        return result;
    } catch (...) {
        if (fs::exists(partial)) fs::remove_all(partial, ignored);
        throw;
    }
}

Project load_project(const fs::path& mod) {
    const auto path = mod / L"reskate-music-project.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(narrow(mod.filename().wstring()) + " has no project file (it was made before they existed); "
                                      "make it again from its song files");
    try {
        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
        if (root.value("schema", 0) != 1) throw std::runtime_error("unsupported schema");
        Project project;
        project.name = root.at("name").string();
        project.playlist = root.at("playlist").string();
        project.bitrate = root.at("bitrate").get<int>();
        project.normalize = root.contains("normalize") ? root.at("normalize").get<bool>() : true;
        if (root.contains("playlist_artwork"))
            for (const auto& [name, imagePath] : root.at("playlist_artwork").items())
                project.playlist_artwork[name] = fs::path(widen(imagePath.string()));
        if (root.contains("generated_playlist_artwork"))
            for (const auto& name : root.at("generated_playlist_artwork")) project.generated_playlist_artwork.insert(name.string());
        for (const auto& song : root.at("songs")) {
            SongInfo info{fs::path(widen(song.at("file").string())), song.at("artist").string(), song.at("title").string()};
            if (song.contains("playlist")) info.playlist = song.at("playlist").string();
            if (song.contains("artwork")) info.artwork = fs::path(widen(song.at("artwork").string()));
            project.songs.push_back(std::move(info));
        }
        return project;
    } catch (const std::exception& error) {
        throw std::runtime_error(narrow(path.wstring()) + " is not a valid project: " + error.what());
    }
}

namespace {
std::vector<std::byte> create_default_icon_png() {
    constexpr std::uint32_t w = 256, h = 256;
    std::vector<std::uint8_t> raw(h * (1 + w * 3));
    std::size_t p = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        raw[p++] = 0; // Filter: None
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto r = static_cast<std::uint8_t>(40 + (x * 60) / 255);
            const auto g = static_cast<std::uint8_t>(30 + (y * 40) / 255);
            const auto b = static_cast<std::uint8_t>(80 + ((x + y) * 120) / 510);
            raw[p++] = r; raw[p++] = g; raw[p++] = b;
        }
    }

    const auto crc32 = [](std::span<const std::uint8_t> data) -> std::uint32_t {
        std::uint32_t crc = 0xFFFFFFFF;
        for (const auto b : data) {
            crc ^= b;
            for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
        return crc ^ 0xFFFFFFFF;
    };

    const auto adler32 = [](std::span<const std::uint8_t> data) -> std::uint32_t {
        std::uint32_t a = 1, b = 0;
        for (const auto x : data) {
            a = (a + x) % 65521;
            b = (b + a) % 65521;
        }
        return (b << 16) | a;
    };

    std::vector<std::uint8_t> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    constexpr std::size_t chunkSize = 32768;
    for (std::size_t i = 0; i < raw.size(); i += chunkSize) {
        const auto end = std::min(i + chunkSize, raw.size());
        const auto len = static_cast<std::uint16_t>(end - i);
        const auto last = (end == raw.size());
        zlib.push_back(last ? 0x01 : 0x00);
        zlib.push_back(static_cast<std::uint8_t>(len & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        const auto nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + i, raw.begin() + end);
    }
    const auto adler = adler32(raw);
    for (int s = 24; s >= 0; s -= 8) zlib.push_back(static_cast<std::uint8_t>((adler >> s) & 0xFF));

    const auto make_chunk = [&](std::string_view type, std::span<const std::uint8_t> payload, std::vector<std::byte>& out) {
        const auto len = static_cast<std::uint32_t>(payload.size());
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>((len >> s) & 0xFF));
        std::vector<std::uint8_t> toCrc(type.begin(), type.end());
        toCrc.insert(toCrc.end(), payload.begin(), payload.end());
        for (const auto b : toCrc) out.push_back(static_cast<std::byte>(b));
        const auto crc = crc32(toCrc);
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>((crc >> s) & 0xFF));
    };

    std::vector<std::byte> png{std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
                               std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};

    std::uint8_t ihdr[13]{
        0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01, 0x00,
        8, 2, 0, 0, 0
    };
    make_chunk("IHDR", ihdr, png);
    make_chunk("IDAT", zlib, png);
    make_chunk("IEND", {}, png);
    return png;
}

std::string thunderstore_name(std::string_view in) {
    std::string out;
    for (const auto c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
        else if (c == ' ' || c == '-') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Mod" : out;
}

std::string generate_readme(const std::string& modName, const std::string& description, const fs::path& modFolder, bool credit = true) {
    std::string md = "# " + modName + "\n\n";
    if (!description.empty()) md += description + "\n\n";
    else md += "Adds custom music to skate. through ReSkate.\n\n";

    const auto jsonPath = modFolder / L"reskate-music.json";
    std::error_code ec;
    if (fs::exists(jsonPath, ec)) {
        try {
            std::ifstream in(jsonPath, std::ios::binary);
            if (in) {
                const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
                if (root.contains("playlists") && root.at("playlists").is_array()) {
                    md += "## Playlists & Tracklist\n\n";
                    for (const auto& pl : root.at("playlists")) {
                        const auto pName = pl.value("name", "Playlist");
                        md += "### " + pName + "\n\n";
                        if (pl.contains("songs") && pl.at("songs").is_array()) {
                            for (const auto& song : pl.at("songs")) {
                                if (song.is_string()) md += "- " + song.string() + "\n";
                            }
                            md += "\n";
                        }
                    }
                }
            }
        } catch (...) {}
    }

    md += "## Installation\n\nInstall via Thunderstore Mod Manager, or drop into your ReSkate launcher.\n";
    if (credit) {
        md += "\n---\n*Packaged with [ReSkate Music Packer](https://github.com/DeckardDetribine/ReSkateMusicPacker)*\n";
    }
    return md;
}
} // namespace

fs::path export_thunderstore(const fs::path& modFolder, const ThunderstoreOptions& options) {
    if (!fs::exists(modFolder / L"layout.toc"))
        throw std::runtime_error("Folder is not a built ReSkate mod (missing layout.toc): " + narrow(modFolder.wstring()));

    std::string modName = "ReSkateMusic";
    try {
        if (fs::exists(modFolder / L"reskate-music-project.json")) {
            const auto proj = load_project(modFolder);
            if (!proj.name.empty()) modName = proj.name;
        } else if (fs::exists(modFolder / L"manifest.json")) {
            std::ifstream in(modFolder / L"manifest.json", std::ios::binary);
            const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
            if (root.contains("name")) modName = root.at("name").string();
        }
    } catch (...) {}

    const auto author = thunderstore_name(options.author.empty() ? "Author" : options.author);
    const auto name = thunderstore_name(modName);
    std::string version = options.version.empty() ? "1.0.0" : options.version;
    if (std::count(version.begin(), version.end(), '.') != 2) version = "1.0.0";

    // A fixed name would be stomped by a second packer process, or by an export running while a
    // build is in flight, so the name is fresh every time and a guard takes it away again even
    // when the packaging below throws.
    const auto staging = scratch_folder() / widen("thunderstore_staging_" + to_hex(new_guid().bytes));
    std::error_code ec;
    fs::create_directories(staging, ec);
    struct Staging { fs::path path; ~Staging() { std::error_code ignored; fs::remove_all(path, ignored); } } stagingGuard{staging};

    // Resolved before the copy loop, so a zip left beside the mod by an earlier export cannot be
    // swept into the next one when the output folder is inside the mod.
    const auto zipName = widen(author + "-" + name + "-" + version + ".zip");
    fs::path outZip = options.output;
    if (outZip.empty()) {
        outZip = modFolder.parent_path() / zipName;
    } else if (fs::is_directory(outZip, ec)) {
        outZip = outZip / zipName;
    }
    fs::create_directories(outZip.parent_path(), ec);
    const auto zipTarget = outZip.lexically_normal();

    for (const auto& entry : fs::directory_iterator(modFolder)) {
        // Anything holding the zip is holding a previous package: skip that whole subfolder.
        const auto inside = zipTarget.lexically_relative(entry.path().lexically_normal());
        if (!inside.empty() && *inside.begin() != L"..") continue;
        auto leaf = entry.path().filename().wstring();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::towlower);
        if (leaf == L"manifest.json" || leaf == L"icon.png" || leaf == L"readme.md" ||
            leaf == L"reskate-music-project.json")
            continue;
        fs::copy(entry.path(), staging / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }

    std::string desc = options.description;
    if (desc.empty()) desc = "Adds music to skate. through ReSkate.";
    if (desc.size() > 250) desc.resize(250);

    const auto text = [](const std::string& s) {
        return std::as_bytes(std::span(s.data(), s.size()));
    };

    auto manifest = Json::object();
    manifest["name"] = name;
    manifest["version_number"] = version;
    manifest["website_url"] = "";
    manifest["description"] = desc;
    manifest["dependencies"] = Json::array();
    write_file(staging / L"manifest.json", text(manifest.dump(2) + "\n"));

    const auto readme = generate_readme(name, desc, modFolder, options.readme_credit);
    write_file(staging / L"README.md", text(readme));

    if (!options.icon.empty() && fs::exists(options.icon)) {
        bool converted = false;
        try {
            constexpr char icon_filter[] =
                "scale=256:256:force_original_aspect_ratio=decrease,pad=256:256:(ow-iw)/2:(oh-ih)/2:color=black@0,setsar=1";
            run({"ffmpeg", "-y", "-v", "error", "-i", path_utf8(options.icon), "-an", "-vf", icon_filter,
                 "-frames:v", "1", "-update", "1", path_utf8(staging / "icon.png")});
            if (fs::is_regular_file(staging / L"icon.png") && fs::file_size(staging / L"icon.png") > 0) {
                converted = true;
            }
        } catch (...) {}
        if (!converted) {
            fs::copy_file(options.icon, staging / L"icon.png", fs::copy_options::overwrite_existing, ec);
        }
    } else if (fs::exists(modFolder / L"icon.png")) {
        fs::copy_file(modFolder / L"icon.png", staging / L"icon.png", fs::copy_options::overwrite_existing, ec);
    } else {
        write_file(staging / L"icon.png", create_default_icon_png());
    }

    if (fs::exists(outZip, ec)) fs::remove(outZip, ec);

    zip_write(outZip, staging);

    if (!fs::exists(outZip, ec))
        throw std::runtime_error("Failed to create Thunderstore package at " + narrow(outZip.wstring()));

    return outZip;

}

} // namespace music
