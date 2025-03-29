# Kamakura Browser

[![Build Status](https://img.shields.io/github/actions/workflow/status/your-username/kamakura-browser/ci-build.yml?style=flat-square)](https://github.com/your-username/kamakura-browser/actions)
[![License](https://img.shields.io/github/license/your-username/kamakura-browser?style=flat-square)](LICENSE)
[![Issues](https://img.shields.io/github/issues/your-username/kamakura-browser?style=flat-square)](https://github.com/your-username/kamakura-browser/issues)
[![Forks](https://img.shields.io/github/forks/your-username/kamakura-browser?style=flat-square)](https://github.com/your-username/kamakura-browser/network)
[![Stars](https://img.shields.io/github/stars/your-username/kamakura-browser?style=flat-square)](https://github.com/your-username/kamakura-browser/stargazers)

**Kamakura Browser** is a lightweight yet powerful browser written in C, focused on protecting your online privacy. It blocks intrusive trackers and avoids harvesting any user data, giving you full control over your web experience.

![Kamakura Browser Screenshot](screen.png)

---

## Features

- **Lightweight:** Built with efficiency in mind, minimizing resource usage.
- **Privacy-first:** No data collection or user tracking. Ever.
- **Tracker Blocking:** Defends against known tracking domains to keep your browsing private.
- **Fast & Minimal:** Strips away unnecessary features to deliver a snappy, straightforward experience.
- **Open Source:** Everyone is free to inspect, modify, and distribute Kamakura Browser.

---

## Getting Started

### Prerequisites
- A C compiler (GCC, Clang, or any other standards-compliant compiler).
- [CMake](https://cmake.org/) (optional, but recommended for easier builds).
- Basic development tools (make, etc.) depending on your platform.

### Building from Source

```bash
# 1. Clone the repository
git clone https://github.com/your-username/kamakura-browser.git
cd kamakura-browser

# 2. Build using CMake (recommended)
cmake -B build
cmake --build build

# 3. Alternatively, build with a compiler directly
gcc src/*.c -o kamakura-browser `pkg-config --cflags --libs gtk+-3.0`
