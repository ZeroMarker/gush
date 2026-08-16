# Gush - BitTorrent Client MVP

A minimal BitTorrent client implemented in modern C++17.

## Features

- Parse .torrent files (bencode decoding)
- **Magnet link support (BEP 9)** with automatic metadata download
- **Dynamic tracker list fetching** from online sources (trackerslist.com, ngosang/trackerslist)
- HTTP/HTTPS/UDP tracker communication (BEP 15)
- DHT peer discovery with iterative `get_peers` queries (BEP 5)
- **Tracker strategy**: exponential failure backoff, success priority, bounded attempts per cycle
- Peer connection and handshake
- BitTorrent peer wire protocol (BEP 3)
- Peer exchange via `ut_pex` (BEP 10/11)
- Piece SHA1 verification before writing to disk
- Rarest-first piece selection with partial-completion priority
- **Endgame mode**: duplicate block requests near completion so slow peers cannot stall the finish
- **Peer health management**: slow/failing peers are evicted and temporarily blacklisted
- Single- and multi-file downloads with path-traversal-safe filenames
- Byte-accurate progress display with live speed
- Piece-verified resume for existing single- and multi-file downloads

## Dependencies

- CMake 3.14+
- OpenSSL (for SHA1 hashing)
- libcurl (for HTTP tracker requests)

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Usage

```
gush [options] <torrent_file|magnet_link> [output_directory]
```

### Options

| Option | Description |
| ------ | ----------- |
| `-o, --download-dir <dir>` | Directory to save downloaded files (default: current directory) |
| `--max-peers <n>` | Maximum concurrent peer connections (default: 30, range 1-500) |
| `--no-tracker-refresh` | Do not fetch tracker lists from the internet |
| `--overwrite` | Discard existing output instead of verifying and resuming it |
| `-v, --verbose` | Verbose logging (tracker/peer details) |
| `-h, --help` | Show help |

### Examples

```bash
# Download using torrent file
./gush ubuntu-22.04.torrent ~/Downloads

# Download using magnet link
./gush "magnet:?xt=urn:btih:HASH&dn=Name" ~/Downloads

# With options
./gush -o ~/Downloads --max-peers 50 "magnet:?xt=urn:btih:HASH"
./gush --no-tracker-refresh ubuntu-22.04.torrent

# Discard an existing partial download and start over
./gush --overwrite -o ~/Downloads ubuntu-22.04.torrent
```

### Resume and overwrite behavior

Resume is enabled by default. Before contacting trackers or peers, gush opens
existing output files without truncating them and verifies every piece against
the SHA1 hashes in the torrent metadata. Verified pieces count toward progress
and are not requested again; missing or corrupt pieces are downloaded normally.
This works when a piece crosses file boundaries in a multi-file torrent.

Output files are resized to the lengths declared by the torrent before
verification. Startup can therefore take noticeable time for large existing
downloads because all stored data must be read and hashed. Use `--overwrite` to
truncate the output and start from zero instead. To protect unrelated data,
resume mode refuses to truncate an existing file that is larger than the
torrent declares; use `--overwrite` explicitly if that replacement is intended.

### Magnet Link Support

When using magnet links, gush will:
1. Parse the magnet link to extract the info hash
2. Automatically fetch the latest tracker list from online sources
3. Discover additional peers through DHT when tracker results are sparse
4. Download metadata from peers using BEP 9 (Extension for Peers to Send Metadata Files)
5. Start downloading the actual content with tracker, DHT and PEX discovery

## Project Structure

```
gush/
├── include/
│   ├── bencode.h           # Bencode parser/encoder
│   ├── torrent.h           # Torrent file parsing
│   ├── tracker.h           # Tracker protocol (HTTP/HTTPS/UDP)
│   ├── tracker_manager.h   # Tracker strategy: backoff + priority
│   ├── dht.h               # BEP 5 peer discovery
│   ├── peer.h              # Peer connection handling
│   ├── downloader.h        # Main download logic
│   ├── metadata.h          # BEP 9 metadata download
│   └── utils.h             # Utilities + leveled logger
└── src/
    ├── main.cpp            # Entry point + CLI
    ├── bencode.cpp
    ├── torrent.cpp
    ├── tracker.cpp
    ├── tracker_manager.cpp
    ├── dht.cpp
    ├── peer.cpp
    ├── downloader.cpp
    ├── metadata.cpp
    ├── magnet.cpp
    ├── tracker_list.cpp
    └── utils.cpp
```

## Testing

```bash
cd build
ctest --output-on-failure
```

Unit tests cover bencode, magnet links, torrent parsing, tracker and DHT responses,
peer protocols, downloader integration, verified resume, overwrite behavior and
utilities. A CI workflow (`.github/workflows/ci.yml`) runs the suite under Release
and ASan/UBSan builds.

## Limitations (MVP)

- DHT currently supports IPv4 peer discovery only (no BEP 5 announce/listener mode)
- Resume verifies the complete existing payload at startup; no fast-resume sidecar is stored
- No encryption support

## License

MIT
