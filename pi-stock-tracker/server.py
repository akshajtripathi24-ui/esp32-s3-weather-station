"""Pi Stock Tracker - a small web app that tracks every US stock.

Run:  python server.py   then open http://<your-pi-ip>:8000
Needs only the `requests` package (pip install requests).
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
INDICES = [("^GSPC", "S&P 500"), ("^IXIC", "Nasdaq"), ("^DJI", "Dow Jones"), ("^RUT", "Russell 2000")]

# Chart range buttons -> (Yahoo range, Yahoo interval)
CHART_RANGES = {"1d": ("1d", "5m"), "5d": ("5d", "30m"), "1m": ("1mo", "1d"), "6m": ("6mo", "1d"),
                "ytd": ("ytd", "1d"), "1y": ("1y", "1d"), "5y": ("5y", "1wk"), "max": ("max", "1mo")}
# Performance periods: trading days back from the latest close (ytd/1y handled specially)
PERIOD_DAYS = {"1w": 5, "1m": 21, "6m": 126}

SCREENER_REFRESH = 300        # all-stock prices every 5 minutes
HISTORY_MAX_AGE = 20 * 3600   # re-crawl a stock's history about once a day
CRAWL_DELAY = 0.6             # pause between Yahoo requests in the background crawl
CACHE_SECONDS = 300

stocks = {}      # symbol -> latest price info for every US stock
history = {}     # symbol -> reference closes for 1W/1M/6M/YTD/1Y changes
profiles = {}    # symbol -> company profile (CEO, employees, description...)
cache = {}       # small in-memory cache for charts and index quotes
status = {"updated": None, "source": None, "crawl_done": 0, "crawl_total": 0, "profiles_ok": True}
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
        fresh[sym] = {
            "symbol": sym,
            "name": (row.get("name") or sym).strip(),
            "price": num(row.get("lastsale")),
            "netchange": num(row.get("netchange")),
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
                          "1d": None, "volume": None, "cap": None, "sector": "Other", "industry": "",
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


# ---------- API ----------

def row_for(s):
    h = history.get(s["symbol"], {})
    price = s["price"]
    out = dict(s)
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


def api_market():
    def one(sym, name):
        def load():
            meta, points = yahoo_chart(sym, "1mo", "1d")
            if not points or meta == "rate-limited":
                return None
            closes = [c for _, c in points]
            price = meta.get("regularMarketPrice") or closes[-1]
            prev = closes[-2] if len(closes) > 1 else closes[-1]
            return {"symbol": sym, "name": name, "price": price, "change": pct(price, prev), "spark": closes}
        return cached("idx:" + sym, 120, load)
    return [x for x in (one(s, n) for s, n in INDICES) if x]


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
    return dict(status, stocks=n, profiles=len(profiles))


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
    print(f"Website running! Open http://<your-pi-ip>:{PORT}  (Ctrl+C to stop)")
    print("Loading all US stocks... the first price download takes a few seconds.")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
