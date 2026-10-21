# Pi Stock Tracker

A small web app for a Raspberry Pi that tracks every US-listed stock (about 7,000), with no extra hardware.

- **Market page:** S&P 500, Nasdaq, Dow and Russell 2000, plus the top gainers and losers for 1D, 1W, 1M, 6M, YTD and 1Y. You can filter by company size. It also lists the most active stocks and shows how each sector did today.
- **Screener:** every US stock in one table. Filter by name, sector and size, sort by any column, and page through 50 at a time.
- **Search:** find a stock by company name, ticker or CEO name.
- **Stock pages:** an interactive chart (1D to Max), performance, key stats, and a company profile with CEO, employees, headquarters, website and a description.
- **Favorites:** tap ☆ on any stock. Favorites are saved in the browser on each device.
- **Light and dark mode**, and the layout works on phones.

Prices come from Nasdaq's public stock screener and refresh every 5 minutes. Charts, history and company profiles come from Yahoo Finance. After the first start, the Pi spends a couple of hours slowly downloading one year of history for every stock, biggest companies first. Until then, the week, month and year columns fill in gradually. The history is saved in `history.json` and `profiles.json`, so restarting doesn't lose it.

The data is for personal use only, and nothing here is investment advice.

## Install on the Pi

```bash
mkdir -p ~/stocktracker && cd ~/stocktracker
python3 -m venv venv && source venv/bin/activate && pip install requests
BASE=https://raw.githubusercontent.com/akshajtripathi24-ui/esp32-s3-weather-station/claude/raspberry-pi-projects-no-hardware-rc5gau/pi-stock-tracker
curl -fsSL -O "$BASE/server.py" -O "$BASE/index.html" -O "$BASE/setup-autostart.sh"
python server.py
```

Then open `http://<your-pi-ip>:8000`. To find the IP address, run `hostname -I`.

To keep it running all the time and start it on boot, run `bash setup-autostart.sh`.

It runs on Python 3.9 and newer, and only needs the `requests` package.
