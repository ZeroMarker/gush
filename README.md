# Gush - BitTorrent Client MVP

A minimal BitTorrent client implemented in modern C++17.

## Features

- Parse .torrent files (bencode decoding)
- **Magnet link support (BEP 9)** with automatic metadata download
- **Dynamic tracker list fetching** from online sources (trackerslist.com, ngosang/trackerslist)
- HTTP/HTTPS/UDP tracker communication
- Peer connection and handshake
- BitTorrent peer wire protocol
- Piece downloading and file assembly
- Progress display

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

```bash
./gush <torrent_file|magnet_link> [output_directory]
```

### Examples

```bash
# Download using torrent file
./gush ubuntu-22.04.torrent ~/Downloads

# Download using magnet link
./gush "magnet:?xt=urn:btih:HASH&dn=Name" ~/Downloads

# Download with automatic tracker list refresh
./gush "magnet:?xt=urn:btih:08ada5a7a6183aae1e09d831df6748d566095a10" ./downloads
```

### Magnet Link Support

When using magnet links, gush will:
1. Parse the magnet link to extract the info hash
2. Automatically fetch the latest tracker list from online sources
3. Download metadata from peers using BEP 9 (Extension for Peers to Send Metadata Files)
4. Start downloading the actual content

## Project Structure

```
gush/
├── CMakeLists.txt          # Build configuration
├── include/
│   ├── bencode.h           # Bencode parser/encoder
│   ├── torrent.h           # Torrent file parsing
│   ├── tracker.h           # Tracker communication
│   ├── peer.h              # Peer connection handling
│   ├── downloader.h        # Main download logic
│   └── utils.h             # Utility functions
└── src/
    ├── main.cpp            # Entry point
    ├── bencode.cpp
    ├── torrent.cpp
    ├── tracker.cpp
    ├── peer.cpp
    ├── downloader.cpp
    └── utils.cpp
```

## Limitations (MVP)

- DHT not yet implemented (relies on trackers for peer discovery)
- Single file download optimization
- Basic piece selection (rarest-first with partial completion priority)
- No peer exchange (PEX)
- No encryption support

## License

MIT
