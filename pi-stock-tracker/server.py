"""Pi Stock Tracker - a small web app that tracks every US stock.

Run:  python server.py   then open http://<your-pi-ip>:8000
Needs: pip install requests websocket-client
Live prices: put your free Finnhub API key in finnhub_key.txt next to this file.
"""
import csv, io, json, os, re, threading, time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import requests

PORT = 8000
HERE = os.path.dirname(os.path.abspath(__file__))
HISTORY_FILE = os.path.join(HERE, "history.json")
PROFILES_FILE = os.path.join(HERE, "profiles.json")
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")
HEADERS = {"User-Agent": UA, "Accept": "application/json, text/plain, */*"}

SCREENER_URL = "https://api.nasdaq.com/api/screener/stocks?tableonly=true&limit=25&offset=0&download=true"
SYMBOL_LISTS = [
    ("https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt", "Symbol"),
    ("https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt", "ACT Symbol"),
]
CHART_URL = "https://query1.finance.yahoo.com/v8/finance/chart/{}"
# Index, display name, and the fund that tracks it (streamed live to estimate the index between updates)
INDICES = [("^GSPC", "S&P 500", "SPY"), ("^NDX", "Nasdaq 100", "QQQ"),
           ("^DJI", "Dow Jones", "DIA"), ("^RUT", "Russell 2000", "IWM")]
FINNHUB_WS = "wss://ws.finnhub.io?token={}"
LIVE_MAX = 50        # Finnhub's free plan streams up to 50 symbols at once
LIVE_IDLE = 30       # stop streaming a stock 30 s after nobody is looking at it

# Chart range buttons -> (Yahoo range, Yahoo interval)
CHART_RANGES = {"1d": ("1d", "5m"), "5d": ("5d", "30m"), "1m": ("1mo", "1d"), "6m": ("6mo", "1d"),
                "ytd": ("ytd", "1d"), "1y": ("1y", "1d"), "5y": ("5y", "1wk"), "max": ("max", "1mo")}
# Performance periods: trading days back from the latest close (ytd/1y handled specially)
PERIOD_DAYS = {"1w": 5, "1m": 21, "6m": 126}

SCREENER_REFRESH = 60         # all-stock prices every minute
HISTORY_MAX_AGE = 20 * 3600   # re-crawl a stock's history about once a day
CRAWL_DELAY = 0.6             # pause between Yahoo requests in the background crawl
CACHE_SECONDS = 300

stocks = {}      # symbol -> latest price info for every US stock
history = {}     # symbol -> reference closes for 1W/1M/6M/YTD/1Y changes
profiles = {}    # symbol -> company profile (CEO, employees, description...)
cache = {}       # small in-memory cache for charts and index quotes
status = {"updated": None, "updated_ts": 0, "source": None, "crawl_done": 0, "crawl_total": 0, "profiles_ok": True}
live = {}        # Finnhub symbol -> {"p": last trade price, "t": trade time (epoch seconds)}
live_wanted = {} # our symbol -> last time a browser asked for it
live_state = {"enabled": False, "connected": False, "subscribed": set(), "error": None}
lock = threading.Lock()


# ---------- helpers ----------

def num(text):
    """'$1,234.50' / '-0.59%' / '' -> float or None"""
    try:
        return float(str(text).replace("$", "").replace(",", "").replace("%", "").strip())
    except ValueError:
        return None


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(dict(data), f)  # dict() makes a safe snapshot while other threads keep writing
    os.replace(tmp, path)


def cached(key, seconds, fn):
    hit = cache.get(key)
    if hit and time.time() - hit[0] < seconds:
        return hit[1]
    value = fn()
    if value is not None:
        cache[key] = (time.time(), value)
    return value


def yahoo_chart(sym, rng="1y", interval="1d"):
    """Return (meta, [(timestamp, close), ...]) or (None, []) if unavailable."""
    try:
        r = requests.get(CHART_URL.format(sym.replace(".", "-").replace("/", "-")),
                         params={"range": rng, "interval": interval}, headers=HEADERS, timeout=15)
        if r.status_code == 429:
            return "rate-limited", []
        result = r.json()["chart"]["result"] if r.status_code == 200 else None
    except (requests.RequestException, ValueError, KeyError, TypeError):
        return None, []
    if not result:
        return None, []
    res = result[0]
    closes = (res.get("indicators", {}).get("quote") or [{}])[0].get("close") or []
    points = [(t, c) for t, c in zip(res.get("timestamp") or [], closes) if c is not None]
    return res.get("meta", {}), points


def pct(now, then):
    if now is None or not then:
        return None
    return round((now / then - 1) * 100, 2)


# ---------- all US stocks (Nasdaq screener) ----------

def load_screener():
    r = requests.get(SCREENER_URL, headers=HEADERS, timeout=30)
    rows = r.json()["data"]["rows"]
    fresh = {}
    for row in rows:
        sym = (row.get("symbol") or "").strip().upper()
        if not sym:
            continue
        price, net = num(row.get("lastsale")), num(row.get("netchange"))
        fresh[sym] = {
            "symbol": sym,
            "name": (row.get("name") or sym).strip(),
            "price": price,
            "netchange": net,
            "prev": round(price - net, 4) if price is not None and net is not None and price - net > 0 else None,
            "1d": num(row.get("pctchange")),
            "volume": num(row.get("volume")),
            "cap": num(row.get("marketCap")),
            "sector": (row.get("sector") or "").strip() or "Other",
            "industry": (row.get("industry") or "").strip(),
            "country": (row.get("country") or "").strip(),
            "ipo": (row.get("ipoyear") or "").strip(),
        }
    return fresh


def load_symbol_lists():
    """Fallback if the Nasdaq screener is down: names only, no prices."""
    fresh = {}
    for url, col in SYMBOL_LISTS:
        text = requests.get(url, headers=HEADERS, timeout=30).text
        for row in csv.DictReader(io.StringIO(text), delimiter="|"):
            sym = row.get(col) or ""
            if not sym or sym.startswith("File Creation") or row.get("Test Issue") == "Y":
                continue
            fresh[sym] = {"symbol": sym, "name": row["Security Name"], "price": None, "netchange": None,
                          "prev": None, "1d": None, "volume": None, "cap": None, "sector": "Other", "industry": "",
                          "country": "", "ipo": ""}
    return fresh


def refresh_prices_forever():
    while True:
        fresh, source = {}, None
        try:
            fresh, source = load_screener(), "Nasdaq"
        except Exception as e:  # network error, format change...
            print("Nasdaq screener failed:", e)
            if not stocks:
                try:
                    fresh, source = load_symbol_lists(), "symbol list (no prices)"
                except Exception as e2:
                    print("Symbol list failed too:", e2)
        if fresh:
            with lock:
                stocks.clear()
                stocks.update(fresh)
            status["updated"] = datetime.now().strftime("%Y-%m-%d %H:%M")
            status["updated_ts"] = time.time()
            status["source"] = source
            print(f"[{status['updated']}] Prices refreshed: {len(fresh)} stocks from {source}")
        time.sleep(SCREENER_REFRESH if source == "Nasdaq" else 60)


# ---------- background crawl: history + company profiles ----------

def history_refs(points):
    """Reference closes we compare today's price against."""
    closes = [c for _, c in points]
    refs = {k: closes[-1 - d] for k, d in PERIOD_DAYS.items() if len(closes) > d}
    refs["1y"] = closes[0]
    year = datetime.now().year
    prev_year = [c for t, c in points if datetime.fromtimestamp(t).year < year]
    if prev_year:
        refs["ytd"] = prev_year[-1]
    refs["hi"], refs["lo"] = max(closes), min(closes)
    refs["t"] = time.time()
    return refs


class ProfileFetcher:
    """Yahoo needs a cookie + 'crumb' for company profiles. If that stops working we just skip profiles."""
    def __init__(self):
        self.session, self.crumb, self.failures = None, None, 0

    def _login(self):
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": UA})
        try:
            self.session.get("https://fc.yahoo.com", timeout=10)
        except requests.RequestException:
            pass
        r = self.session.get("https://query1.finance.yahoo.com/v1/test/getcrumb", timeout=10)
        self.crumb = r.text.strip() if r.status_code == 200 and "<" not in r.text else None

    def fetch(self, sym):
        if self.failures >= 5:
            status["profiles_ok"] = False
            return None
        try:
            if not self.crumb:
                self._login()
            if not self.crumb:
                self.failures += 1
                return None
            r = self.session.get(
                "https://query2.finance.yahoo.com/v10/finance/quoteSummary/" + sym.replace(".", "-"),
                params={"modules": "assetProfile", "crumb": self.crumb}, timeout=15)
            if r.status_code in (401, 403):
                self.crumb = None
                self.failures += 1
                return None
            result = r.json()["quoteSummary"]["result"]
        except (requests.RequestException, ValueError, KeyError, TypeError):
            return None
        self.failures = 0
        if not result:
            return {}
        p = result[0].get("assetProfile", {})
        officers = p.get("companyOfficers") or []
        ceo = next((o for o in officers if re.search(r"\bCEO\b|Chief Executive", o.get("title", ""), re.I)),
                   officers[0] if officers else None)
        return {
            "ceo": ceo.get("name") if ceo else None,
            "ceo_title": ceo.get("title") if ceo else None,
            "employees": p.get("fullTimeEmployees"),
            "website": p.get("website"),
            "hq": ", ".join(x for x in (p.get("city"), p.get("state"), p.get("country")) if x),
            "summary": (p.get("longBusinessSummary") or "")[:900],
        }


quote_fetcher = ProfileFetcher()  # for stocks opened before the crawl reaches them


def crawl_forever():
    history.update(load_json(HISTORY_FILE))
    profiles.update(load_json(PROFILES_FILE))
    fetcher = ProfileFetcher()
    while not stocks:
        time.sleep(2)
    while True:
        with lock:
            order = sorted(stocks.values(), key=lambda s: -(s["cap"] or 0))
        syms = [s["symbol"] for s in order]
        status["crawl_total"] = len(syms)
        status["crawl_done"] = sum(1 for s in syms if s in history)
        changed = 0
        for sym in syms:
            h = history.get(sym)
            if h and time.time() - h.get("t", 0) < HISTORY_MAX_AGE:
                continue
            meta, points = yahoo_chart(sym, "1y", "1d")
            if meta == "rate-limited":
                print("Yahoo asked us to slow down - pausing 2 minutes")
                time.sleep(120)
                continue
            if sym not in history:
                status["crawl_done"] += 1
            history[sym] = history_refs(points) if points else {"t": time.time()}
            if sym not in profiles and status["profiles_ok"]:
                prof = fetcher.fetch(sym)
                if prof is not None:
                    profiles[sym] = prof
            changed += 1
            if changed % 50 == 0:
                save_json(HISTORY_FILE, history)
                save_json(PROFILES_FILE, profiles)
            time.sleep(CRAWL_DELAY)
        if changed:
            save_json(HISTORY_FILE, history)
            save_json(PROFILES_FILE, profiles)
        time.sleep(600)


# ---------- live prices (Finnhub WebSocket) ----------

def read_key():
    key = os.environ.get("FINNHUB_KEY", "").strip()
    if not key:
        try:
            with open(os.path.join(HERE, "finnhub_key.txt")) as f:
                key = f.read().strip()
        except OSError:
            pass
    return key


def fh_symbol(sym):
    """Nasdaq writes class shares as BRK/B, Finnhub as BRK.B"""
    return sym.replace("/", ".").replace("-", ".")


def live_targets():
    """The (at most 50) symbols to stream: index funds first, then what browsers looked at most recently."""
    now = time.time()
    out = [etf for _, _, etf in INDICES]
    for sym, _ in sorted(list(live_wanted.items()), key=lambda x: -x[1]):
        if now - live_wanted.get(sym, 0) > LIVE_IDLE or len(out) >= LIVE_MAX:
            break
        f = fh_symbol(sym)
        if f not in out and "^" not in f:
            out.append(f)
    for sym in [s for s, t in list(live_wanted.items()) if now - t > 600]:
        live_wanted.pop(sym, None)
    return set(out)


def live_forever(key):
    import websocket  # pip install websocket-client

    def sync_subscriptions(ws):
        while live_state["connected"]:
            want, have = live_targets(), live_state["subscribed"]
            try:
                for s in want - have:
                    ws.send(json.dumps({"type": "subscribe", "symbol": s}))
                for s in have - want:
                    ws.send(json.dumps({"type": "unsubscribe", "symbol": s}))
            except Exception:
                return
            live_state["subscribed"] = want
            time.sleep(1)

    def on_open(ws):
        live_state.update(connected=True, error=None, subscribed=set())
        print("Live prices connected (Finnhub)")
        threading.Thread(target=sync_subscriptions, args=(ws,), daemon=True).start()

    def on_message(ws, message):
        try:
            msg = json.loads(message)
        except ValueError:
            return
        if msg.get("type") == "trade":
            for t in msg.get("data") or []:
                live[t["s"]] = {"p": t["p"], "t": t["t"] / 1000}
        elif msg.get("type") == "error":
            live_state["error"] = msg.get("msg")
            print("Finnhub error:", msg.get("msg"))

    def on_error(ws, error):
        text = str(error)
        live_state["error"] = "Finnhub rejected the API key" if "401" in text else text

    def on_close(ws, *args):
        live_state["connected"] = False

    wait = 5
    while True:
        started = time.time()
        ws = websocket.WebSocketApp(FINNHUB_WS.format(key), on_open=on_open, on_message=on_message,
                                    on_error=on_error, on_close=on_close)
        try:
            ws.run_forever(ping_interval=30, ping_timeout=10)
        except Exception as e:
            live_state["error"] = str(e)
        live_state["connected"] = False
        wait = 5 if time.time() - started > 120 else min(wait * 2, 120)
        print(f"Live prices disconnected ({live_state['error'] or 'closed'}) - retrying in {wait}s")
        time.sleep(wait)


def start_live():
    key = read_key()
    if not key:
        live_state["error"] = "No Finnhub key (add finnhub_key.txt for live prices)"
        print(live_state["error"])
        return
    try:
        import websocket  # noqa: F401
    except ImportError:
        live_state["error"] = "Run: pip install websocket-client"
        print("Live prices off -", live_state["error"])
        return
    live_state["enabled"] = True
    threading.Thread(target=live_forever, args=(key,), daemon=True).start()


# ---------- API ----------

def row_for(s):
    h = history.get(s["symbol"], {})
    out = dict(s)
    tick = live.get(fh_symbol(s["symbol"]))
    if tick and tick["t"] >= status["updated_ts"] - 120:   # a trade newer than the last full download
        out["price"], out["live"] = tick["p"], True
        if s.get("prev"):
            out["netchange"] = round(tick["p"] - s["prev"], 4)
            out["1d"] = pct(tick["p"], s["prev"])
    price = out["price"]
    for k in ("1w", "1m", "6m", "ytd", "1y"):
        out[k] = pct(price, h.get(k))
    p = profiles.get(s["symbol"]) or {}
    out["ceo"] = p.get("ceo")
    return out


def api_screener(qs):
    g = lambda k, d="": qs.get(k, [d])[0]
    sector, period = g("sector"), g("period", "1d")
    sort, order = g("sort", period), g("order", "desc")
    mincap, maxcap = num(g("mincap", "0")) or 0, num(g("maxcap", "0")) or 0
    text = g("q").strip().lower()
    symbols = [s for s in g("symbols").upper().split(",") if s]
    offset, limit = int(num(g("offset", "0")) or 0), min(int(num(g("limit", "50")) or 50), 200)
    with lock:
        pool = [stocks[s] for s in symbols if s in stocks] if symbols else list(stocks.values())
    rows = []
    for s in pool:
        if sector and s["sector"] != sector:
            continue
        if mincap and (s["cap"] or 0) < mincap:
            continue
        if maxcap and (s["cap"] or 0) > maxcap:
            continue
        if text and text not in s["symbol"].lower() and text not in s["name"].lower():
            continue
        rows.append(row_for(s))
    if sort in ("symbol", "name", "sector"):
        rows.sort(key=lambda r: r[sort] or "", reverse=order == "desc")
    else:
        have = [r for r in rows if r.get(sort) is not None]
        missing = [r for r in rows if r.get(sort) is None]
        have.sort(key=lambda r: r[sort], reverse=order == "desc")
        rows = have + missing
    return {"total": len(rows), "rows": rows[offset:offset + limit]}


def api_sectors():
    groups = {}
    with lock:
        for s in stocks.values():
            if s["1d"] is None or not s["cap"]:
                continue
            g = groups.setdefault(s["sector"], [0.0, 0.0, 0])
            g[0] += s["1d"] * s["cap"]
            g[1] += s["cap"]
            g[2] += 1
    out = [{"sector": k, "change": round(v[0] / v[1], 2), "cap": v[1], "count": v[2]}
           for k, v in groups.items() if v[1] > 0]
    return sorted(out, key=lambda x: -x["change"])


def yahoo_today(sym):
    """Previous close and latest price from Yahoo."""
    meta, points = yahoo_chart(sym, "1d", "5m")
    if not isinstance(meta, dict):
        return None
    prev = meta.get("chartPreviousClose") or meta.get("previousClose")
    price = meta.get("regularMarketPrice") or (points[-1][1] if points else None)
    return {"prev": prev, "price": price} if prev and price else None


def yahoo_spark(sym):
    meta, points = yahoo_chart(sym, "1mo", "1d")
    return [c for _, c in points] or None


def api_market():
    out = []
    for sym, name, etf in INDICES:
        today = cached("today:" + sym, 30, lambda: yahoo_today(sym))
        if not today:
            continue
        price, is_live = today["price"], False
        # Between Yahoo updates, move the index by the same % as its live-streamed fund
        etf_today = cached("today:" + etf, 600, lambda: yahoo_today(etf))
        tick = live.get(etf)
        if etf_today and tick and time.time() - tick["t"] < 300:
            price, is_live = today["prev"] * tick["p"] / etf_today["prev"], True
        out.append({"symbol": sym, "name": name, "price": price, "change": pct(price, today["prev"]),
                    "live": is_live, "spark": cached("spark:" + sym, 600, lambda: yahoo_spark(sym)) or []})
    return out


def api_live(qs):
    """Latest prices for the stocks a browser has on screen (first = most important)."""
    syms = [s for s in qs.get("symbols", [""])[0].upper().split(",") if s][:200]
    now = time.time()
    with lock:
        pool = [stocks[s] for s in syms if s in stocks]
    for i, s in enumerate(p["symbol"] for p in pool):
        live_wanted[s] = max(live_wanted.get(s, 0), now - i * 0.001)
    quotes = {}
    for s in pool:
        r = row_for(s)
        quotes[s["symbol"]] = {"p": r["price"], "c": r["1d"], "n": r["netchange"], "live": r.get("live", False)}
    return {"connected": live_state["connected"], "enabled": live_state["enabled"],
            "error": live_state["error"], "quotes": quotes}


def api_search(q):
    q = q.strip()
    if not q:
        return []
    up, low = q.upper(), q.lower()
    with lock:
        allrows = list(stocks.values())
    by_cap = lambda s: -(s["cap"] or 0)
    exact = [s for s in allrows if s["symbol"] == up]
    starts = sorted([s for s in allrows if s["symbol"].startswith(up) and s["symbol"] != up], key=by_cap)[:5]
    seen = {s["symbol"] for s in exact + starts}
    names = sorted([s for s in allrows if low in s["name"].lower() and s["symbol"] not in seen], key=by_cap)
    seen |= {s["symbol"] for s in names}
    people = []
    if len(low) >= 3:
        people = sorted([s for s in allrows if s["symbol"] not in seen and
                         low in ((profiles.get(s["symbol"]) or {}).get("ceo") or "").lower()], key=by_cap)
    out = []
    for s in (exact + starts + names[:10] + people[:5])[:15]:
        out.append({"symbol": s["symbol"], "name": s["name"], "price": s["price"], "1d": s["1d"],
                    "ceo": (profiles.get(s["symbol"]) or {}).get("ceo")})
    return out


def api_quote(sym):
    with lock:
        s = stocks.get(sym)
    if not s:
        return None
    out = row_for(s)
    h = history.get(sym, {})
    out["hi52"], out["lo52"] = h.get("hi"), h.get("lo")
    if sym not in profiles and status["profiles_ok"]:
        prof = quote_fetcher.fetch(sym)
        if prof is not None:
            profiles[sym] = prof
    out["profile"] = profiles.get(sym) or {}
    out["profiles_ok"] = status["profiles_ok"]
    return out


def api_chart(sym, rng):
    yr, iv = CHART_RANGES.get(rng, CHART_RANGES["1y"])

    def load():
        meta, points = yahoo_chart(sym, yr, iv)
        if not points or meta == "rate-limited":
            return None
        intraday = iv.endswith("m")
        fmt = "%b %d %H:%M" if intraday else "%Y-%m-%d"
        return {"labels": [datetime.fromtimestamp(t).strftime(fmt) for t, _ in points],
                "closes": [round(c, 4) for _, c in points],
                "price": meta.get("regularMarketPrice")}
    return cached(f"chart:{sym}:{rng}", 60 if rng in ("1d", "5d") else CACHE_SECONDS, load)


def api_status():
    with lock:
        n = len(stocks)
    return dict(status, stocks=n, profiles=len(profiles), live_enabled=live_state["enabled"],
                live_connected=live_state["connected"], live_streaming=len(live_state["subscribed"]),
                live_error=live_state["error"])


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlparse(self.path)
        qs = parse_qs(url.query)
        sym = qs.get("symbol", [""])[0].upper()
        valid = re.fullmatch(r"[A-Z0-9.\-^/]{1,12}", sym) is not None
        try:
            if url.path == "/":
                with open(os.path.join(HERE, "index.html"), "rb") as f:
                    return self.send(f.read(), "text/html; charset=utf-8")
            if url.path == "/api/screener":
                return self.send_json(api_screener(qs))
            if url.path == "/api/sectors":
                return self.send_json(api_sectors())
            if url.path == "/api/market":
                return self.send_json(api_market())
            if url.path == "/api/search":
                return self.send_json(api_search(qs.get("q", [""])[0]))
            if url.path == "/api/status":
                return self.send_json(api_status())
            if url.path == "/api/live":
                return self.send_json(api_live(qs))
            if url.path == "/api/quote":
                data = api_quote(sym) if valid else None
                return self.send_json(data or {"error": "Unknown stock"}, 200 if data else 404)
            if url.path == "/api/chart":
                data = api_chart(sym, qs.get("range", ["1y"])[0]) if valid else None
                return self.send_json(data or {"error": "No chart data"}, 200 if data else 404)
            self.send(b"Not found", "text/plain", 404)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_json(self, obj, status_code=200):
        self.send(json.dumps(obj).encode(), "application/json", status_code)

    def send(self, body, ctype, status_code=200):
        self.send_response(status_code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    threading.Thread(target=refresh_prices_forever, daemon=True).start()
    threading.Thread(target=crawl_forever, daemon=True).start()
    start_live()
    print(f"Website running! Open http://<your-pi-ip>:{PORT}  (Ctrl+C to stop)")
    print("Loading all US stocks... the first price download takes a few seconds.")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
