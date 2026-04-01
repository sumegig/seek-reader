# SEEK Reader

## Motivation

E-paper devices are fantastic for reading, but most commercially available readers are closed systems with limited customization. The **Xteink X4** is an affordable, ESP32-C3 based e-paper device, but its official firmware remains closed.

**SEEK** is a custom, open-source firmware for the Xteink X4. It originally started as a fork of the excellent [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) project but has since evolved into an independent system. 

My primary goal with SEEK is to build a highly customizable, personalized reading experience tailored exactly to my own reading habits and needs. I am continuously tweaking the UI, adding new features, and shaping it into my perfect e-reader. However, if you like the direction this project is heading, you are more than welcome to flash it to your device, use it, or contribute!

*This project is **not affiliated with Xteink**; it's built independently as a personal/community project.*

## What's Inherited vs. What's New

**Inherited from CrossPoint (The Core Engine):**
* Robust EPUB 2 and EPUB 3 parsing and rendering.
* Aggressive SD-card caching mechanism (crucial for the ESP32-C3's 380KB RAM limit).
* WiFi OTA (Over-The-Air) updates and Book upload.
* KOReader Sync integration for cross-device reading progress.
* Multi-language support.

**SEEK Customizations & Features (My Additions):**
* **Detailed Reading Statistics:** A dedicated, E-ink optimized statistics screen tracking all-time reading hours, session counts, and per-book progress.
* **Custom UI & Layouts:** Personalized home screen layouts, cascading cover resolution fallbacks, and tailored navigation.
* **More to come:** I am actively shaping the firmware, so expect more personalized features, custom sleep screens, and UI overhauls.

## Features & Usage

- [x] EPUB parsing and rendering (EPUB 2 and EPUB 3)
- [x] Image support within EPUB
- [x] Saved reading position
- [x] File explorer with nested folder support
- [x] Detailed Reading Statistics & Time Tracking
- [x] Custom sleep screens (Cover sleep screen)
- [x] Wifi book upload & OTA updates
- [x] KOReader Sync integration
- [x] Configurable font, layout, and display options
- [x] Screen rotation (4 orientations)

See the [USER_GUIDE.md](./USER_GUIDE.md) for basic operating instructions.

## Installing (Manual / Development)

Since SEEK is an actively developed custom firmware, the primary way to install it is by compiling and flashing it yourself via USB-C.

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4 device

### Checking out the code

Clone the repository and its submodules:

```sh
git clone --recursive https://github.com/sumegig/seek-reader

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive

```
### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command using PlatformIO:

```Bash
pio run --target upload
```

_(Note: If you ever want to revert back to the official firmware, you can use the web flasher at https://xteink.dve.al/)_

### Debugging

If you are modifying the code, it’s recommended to capture detailed logs from the serial port. Install the required Python packages:

```Bash
python3 -m pip install pyserial colorama matplotlib
```

Then run the monitoring script:

```Bash
# For Linux/Windows (Git Bash)
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

## Internals & RAM Constraints

SEEK (like CrossPoint) is very aggressive about caching data to the SD card to minimize RAM usage. The ESP32-C3 only has **~380KB of usable RAM**, and the E-ink display buffer alone takes up a significant chunk of that.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from this cache. This cache directory exists at `.crosspoint` on the SD card (the folder name is kept for backward compatibility).

The structure is as follows:

```Plaintext
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── cover.bmp        # Book cover images (generated at multiple resolutions)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, layout info, etc.)
│       └── ...
├── stats.bin            # Global and per-book reading statistics
└── system/
    └── BasicCover.bmp   # Fallback placeholder for missing covers
```
Deleting the `.crosspoint` directory will clear the entire cache and force the device to re-parse books upon opening them.

For more details on the internal file structures, see the [file formats document](https://www.google.com/search?q=./docs/file-formats.md).

## Contributing

While SEEK is heavily tailored to my personal use, contributions are very welcome! If you find a bug, want to improve the E-ink rendering, or have an idea that fits the scope of a lightweight reader, feel free to join in.

### To submit a contribution:

1. Fork the repo
    
2. Create a branch (`feature/your-cool-idea`)
    
3. Make your changes (Please adhere to the 380KB RAM constraints and use the HAL!)
    
4. Submit a PR
    

---

**Credits & Acknowledgements**

- Huge thanks to the original **[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader)** project for providing the incredible foundation for this firmware.
    
- Shoutout to [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader), which inspired the original architecture.