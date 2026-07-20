# IPL Scheduling System

A C program that generates a constraint-respecting IPL-style cricket league
schedule for a fixed 10-team roster (KKR, GT, CSK, RCB, SRH, MI, DC, LSG, RR,
PBKS), tracks match results and a points table, and runs the standard 4-match
playoff bracket (Qualifier 1 → Eliminator → Qualifier 2 → Final) to determine
a champion.


- A CGI web app (HTML forms + a C backend) with a styled,
  multi-page browser UI: Setup → Team Fixtures → Stadium Fixtures →
  Results → Playoffs → All-Time Champions.

---

## Table of Contents

1. [Repository structure](#repository-structure)
2. [Features](#features)
3. [Constraints implemented](#constraints-implemented)
4. [Web version — setup & run](#web-version--setup--run)
5. [Web version — page-by-page workflow](#web-version--page-by-page-workflow)
6. [Files generated at runtime](#files-generated-at-runtime)
7. [Troubleshooting](#troubleshooting)
8. [Known limitations](#known-limitations)

---

## Repository structure

```
ipl-repo/
├── .gitignore
├── README.md
├── process.c              # CGI backend — compile this to process.cgi
├── index.html             # Setup page: dates, blackout dates, strengths, holidays
├── fixtures.html          # Look up one team's fixtures
├── stadium.html           # Look up one venue's fixtures
├── results.html           # Enter manual results / trigger simulation
├── playoffs.html          # Run the playoff bracket
├── history.html           # All-time champions + past-season browser
├── style.css               # Shared styling for all the HTML pages
```

---

## Features

**Core scheduling**
- Double round-robin: every team plays every other team home & away (90 matches)
- One match per weekday (7:30 PM), two per weekend day (3:30 PM + 7:30 PM double-header)
- Minimum 2–3 days rest between a team's matches
- No team plays twice in a day; no 3+ consecutive away matches; balanced home/away streaks
- Venue blackout-date support
- Travel-distance-aware opponent selection (haversine distance between real venue coordinates)
- Prime-time / holiday weighting for popular teams and rivalry fixtures

**Results & standings**
- Manual result entry, weighted random simulation (by team strength rating), or a mix of both
- Points table sorted by Points → NRR → Head-to-head
- Full 4-match playoff bracket with proper winner progression to the Final

**History & persistence**
- Multi-season history (`history.csv`): champion + runner-up per season
- Full per-season archiving: schedule, match results, final points table, and
  playoff bracket are all snapshotted the moment a season's playoffs finish,
  browsable later by season number
- CSV file persistence throughout, so the web app survives across separate
  page loads / browser sessions

---

## Constraints implemented

The fixture generation system follows the constraints below to create a realistic and balanced tournament schedule.

| **Constraint** | **Description** |
|----------------|-----------------|
| Double Round Robin | Every team plays every other team twice (one home match and one away match). |
| One Match per Team per Day | A team cannot be scheduled for more than one match on the same day. |
| Balanced Home Matches | Each team plays an equal number of home and away matches over the season. |
| Venue Availability | A venue cannot host more than one match at the same time. |
| Venue Blackout Dates | Matches are not scheduled on dates when a venue is unavailable. |
| Match Time Slots | One match is scheduled on weekdays (7:30 PM), while weekends have two matches (3:30 PM and 7:30 PM). |
| Rest Period | A minimum gap of 2–3 days is maintained between consecutive matches for each team. |
| Away Match Limit | No team is scheduled for more than two consecutive away matches. |
| Home/Away Balance | Home and away fixtures are distributed evenly throughout the season to avoid long streaks. |
| Travel Optimization | The schedule minimizes long-distance travel whenever possible. |
| Prime-Time Fixtures | Popular teams and rivalry matches are preferentially assigned to prime-time television slots. |
| Public Holiday Scheduling | Important matches are prioritized on public holidays to maximize attendance and viewership. |

See [Known limitations](#known-limitations) for the honest caveats on the two
"soft preference" rows above.

---


## Web version — setup & run

### 1. Install a C compiler

If you don't already have `gcc`:
- **Windows:** install MinGW-w64 (https://www.mingw-w64.org/downloads/) and add
  its `bin` folder to your System PATH.
- **Linux/Mac:** `gcc` is usually already available; if not, install via your
  package manager (`apt install gcc`, `brew install gcc`, etc.).

### 2. Compile the CGI backend

```bash
gcc -O2 -Wall -o process.cgi process.c -lm
```

On **Windows**, build statically to avoid missing-DLL errors when Apache
launches the program:
```cmd
gcc -O2 -Wall -static -o process.cgi process.c -lm
```

### 3. Set up a CGI-capable web server

Any CGI-capable server works; these instructions assume **Apache**.

**a. Enable the CGI module.** In `httpd.conf`, make sure this line is
uncommented:
```apache
LoadModule cgi_module modules/mod_cgi.so
```

**b. Enable script execution for the folder** you'll serve `web/` from.
Find (or add) the `<Directory>` block matching your `DocumentRoot`, and make
sure it includes `ExecCGI` and the `.cgi` handler:
```apache
<Directory "${SRVROOT}/htdocs">
    Options Indexes FollowSymLinks ExecCGI
    AddHandler cgi-script .cgi
    AllowOverride None
    Require all granted
</Directory>
```

**c. Copy every file from `web/`** (including `history.csv`) into that
`DocumentRoot` folder, alongside the compiled `process.cgi`.

**d. Validate and restart Apache** (Windows, from an **Administrator**
terminal):
```cmd
cd C:\Apache24\bin
httpd.exe -t
net stop Apache2.4
net start Apache2.4
```

### 4. Open it in a browser

```
http://localhost/index.html
```

---

## Web version — page-by-page workflow

1. **`index.html` (Setup)** — enter start/end dates, optional venue blackout
   dates, optional team strength ratings (1–100, blank = 50), optional
   holiday dates, and a season label. Click **Generate Schedule**. Needs
   roughly a 90-day window for all 90 matches to fit with the realistic
   day-cap and rest-day rules.
2. **`fixtures.html`** — type a team code (e.g. `CSK`) to see just that
   team's 18 fixtures.
3. **`stadium.html`** — type an exact venue name (copy it from the schedule
   table) to see just that venue's matches.
4. **`results.html`** — leave the textarea blank to auto-simulate the whole
   season using your strength ratings, or paste `runs1,runs2[,player-of-match]`
   lines (one per match, **in schedule order**) to override specific results.
   Blank lines fall back to simulation, so manual and simulated results can
   be freely mixed.
5. **`playoffs.html`** — leave the 4 fields blank to auto-decide winners by
   strength, or type known winners once you've seen the actual top 4. Running
   this appends the result to `history.csv` and archives the full season's
   schedule/results/points/playoff data.
6. **`history.html`** — click **View History** for the season-by-season
   record and an all-time title tally, or enter a season number and click
   **View Season Details** to see that season's complete archived schedule,
   match results, points table, and playoff bracket.

---

## Files generated at runtime

| File | Contents | Tracked in git? |
|---|---|---|
| `Roundrobin.csv` | Current season's generated schedule | No |
| `MatchResults.csv` | Current season's per-match scores/winners/POTM | No |
| `file1.csv` | Current season's sorted points table | No |
| `settings.csv` | Season label + strength ratings (persisted between pages) | No |
| `season_report.txt` | Plain-text summary of the latest playoffs run | No |
| `PlayoffResults.csv` | Current season's playoff bracket results | No |
| `history.csv` | One row per completed season: `Season,Champion,RunnerUp` | No |
| `archive_season_N_schedule.csv` | Archived schedule for season N | No |
| `archive_season_N_results.csv` | Archived match results for season N | No |
| `archive_season_N_points.csv` | Archived points table for season N | No |
| `archive_season_N_playoffs.csv` | Archived playoff bracket for season N | No |


---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Browser shows raw binary garbage instead of a page | Apache is serving `process.cgi` as a static file, not executing it | Confirm `mod_cgi` is loaded (`httpd.exe -M \| findstr cgi`) and the correct `<Directory>` block has `ExecCGI` + `AddHandler cgi-script .cgi` |
| `identifier "M_PI" is undefined` at compile time | MinGW's `math.h` doesn't expose `M_PI` by default | Already fixed in `process.c` with a fallback `#define M_PI` |
| `net stop`/`net start` says "Access is denied" | Command Prompt isn't running as Administrator | Right-click Command Prompt → **Run as administrator** |
| "N of 90 fixtures could not be scheduled" warning | Date range too short for the realistic 1–2-matches-per-day cap | Extend the season to roughly 90+ days |
| Points table looks unweighted (pure 50/50) | `settings.csv` missing — visited Results before Setup | Run Setup (**Generate Schedule**) first, in the same folder, before Results |

---

## Known limitations

- Fixed 10-team roster with real IPL city venues — not dynamically
  configurable through the UI.
- Match results are simulated (weighted by team strength) or manually
  entered; there's no real scorecard/live-data integration.
- The scheduler is a single-pass **greedy** algorithm, not a full constraint
  solver. Hard constraints (rest days, blackout dates, day caps, no
  double-booking) are always satisfied. Two "soft" constraints — avoiding a
  2nd consecutive away game, and marquee-fixture prime-time placement — are
  strongly preferred via scoring penalties rather than absolutely guaranteed,
  because a true hard guarantee would require backtracking search. In
  practice this means at most a handful of matches per season involve a
  team's 2nd (never 3rd) consecutive away game.
