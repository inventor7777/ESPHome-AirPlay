# Third-party notices

The original integration also credits these projects:

- [squeezelite-esp32](https://github.com/sle118/squeezelite-esp32) — RTP packet handling and audio buffering.
- [shairport-sync](https://github.com/mikebrady/shairport-sync) — RTSP protocol and authentication.

The RAOP transport and public protocol RSA key derive from the original fork's implementation:

(c) Philippe 2016–2019, philippe_44@outlook.com. Released under the MIT License.

The RTP timing, resend and replay approach derives from HairTunes:

Copyright (c) James Laird 2011. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

ALAC decoding is provided by the separately fetched `espressif/esp_audio_codec` managed component (version 2.5.0), under the **Espressif Modified MIT License**, SPDX identifier `LicenseRef-Espressif-Modified-MIT`.

Copyright (c) 2025 Espressif Systems (Shanghai) CO., LTD.

This is not the standard MIT license: it permits use exclusively with Espressif Systems products and prohibits redistribution for use with non-Espressif products. See the [upstream license and full terms](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.5.0/license). The dependency retains its own license; no decoder binary is bundled in this repository.
