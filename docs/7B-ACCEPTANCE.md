# Waveshare 7B acceptance checklist

Target: ESP32-S3-Touch-LCD-7B, 1024 × 600. Keep the stock 7B installer available for USB recovery. Use the USB-C UART port with the rear switch on UART1.

## Before installing

- Use a successful **Build Waveshare 7B** run for the exact commit under test. The **host-checks** job covers simulated provider switching, retry pacing, refresh bounds, and stale expiry.
- The **waveshare-7b** artifact contains the bootloader, partitions, assets, application, and ESP-IDF flash arguments. The **waveshare-7b-private-update** artifact contains the application and its manifest.
- For a private update server, serve the two private-update files in the same directory over HTTPS with a certificate trusted by the ESP32. Enter the directory URL in **Settings → System**. A newer release must increment the version in the root CMake configuration.

## Optional SD cache

Power the board off before inserting a FAT32 or exFAT microSD card. The firmware never formats it. Reboot and check that `/api/state` shows `sd_cache_ready: true`. Open the radar normally to cache tiles you view. Visit the same view again, then repeat after a restart. If there is no usable card, the radar uses its onboard cache.

To preload a photo you have permission to use, put a JPEG under `radar/photos` on the card. Name it with the aircraft's six-character ICAO hex address, for example `a0b7dc.jpg`, and keep each image under 512 KB. The radar checks that folder first. Successfully downloaded photos and credits are stored separately in `radar/cachep`, and reused across restarts. OpenStreetMap public tiles are cached as you view them under their tile usage policy. Live positions are still fetched from the provider.

## On-device checks

- [ ] Install the 7B build and confirm the panel starts cleanly, touch targets work, and hardware brightness changes.
- [ ] Join a 2.4 GHz network through the touchscreen. Reboot and confirm automatic reconnection.
- [ ] Open **Set up from phone**, join the displayed temporary network, set Wi-Fi credentials, and confirm the radar restarts onto that network.
- [ ] Confirm the initial radar view opens at Home, fits 25 NM, and starts with Follow Map, ground traffic, and map rings off.
- [ ] Set Home using an airport code, coordinates, and a panned map center. Reboot and confirm Home persists.
- [ ] Try each range preset. Pan in Fixed Area mode, then turn on Follow Map and check loading and overlap behavior. Use Home to reframe the selected radius.
- [ ] At wide range or where aircraft icons overlap, check that each group uses a normal aircraft icon. Tap it to zoom in and check that individual callsigns appear where space permits.
- [ ] Tap an aircraft row in the left list and check that a compact card opens. Close it, then hold the same row for two seconds and check that **More** opens directly. Scroll the list to verify scrolling opens neither view. Check that route, airframe and photo complete or show an unavailable state.
- [ ] Change the refresh interval and restart. Exercise all three motion modes.
- [ ] Confirm primary provider and snapshot age on the map. On a controlled router or test network, temporarily block only the primary hostname, observe fallback, then remove the block and observe recovery. Avoid repeated live fallback requests.
- [ ] During an outage of all aircraft providers, verify the display marks the data stale and clears aircraft about 60 seconds after the refresh failure. Map-only outage should leave the basic radar, bundled airports and any available aircraft data.
- [ ] Open the home weather panel and scroll through the 24-hour forecast. Test the manual UTC offset and return to automatic.
- [ ] Set day and night brightness levels and schedule a boundary a few minutes ahead. Adjust brightness manually and verify the schedule resumes at that boundary.
- [ ] Configure a private HTTPS update server, offer a newer 7B build, and verify only tapping **Install** starts the update. Check the new version and saved settings after reboot.

Record the firmware commit, each result, and any error shown. Feature 2.1 passes only after live polling, automatic fallback, and recovery pass on the physical board.
