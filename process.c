/* =========================================================================
   IPL SCHEDULE GENERATOR - CGI BACKEND  (process.c -> compile to process.cgi)
   -------------------------------------------------------------------------
   Fixed 10-team IPL model (KKR, GT, CSK, RCB, SRH, MI, DC, LSG, RR, PBKS)
   with real venues + approximate coordinates.

   Persisted files (server working directory):
     Roundrobin.csv    - generated league schedule
     MatchResults.csv  - per-match scores + winner (manual or simulated)
     file1.csv         - points table (sorted, same 6-col format as before)
     history.csv       - one row per season: Season,Champion,RunnerUp
     season_report.txt - plain-text summary of the latest run

   "action" form field drives what happens (menu-style, since CGI is
   stateless per request but files persist across requests):
     full      (default) - generate schedule + results + points + playoffs
     playoffs             - re-run ONLY the playoffs, reusing existing file1.csv
     history               - just show the all-time champions table
   ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NTEAMS 10
#define MAX_BLACKOUT   20
#define MAX_RESULTS    100
#define LINE_BUF       64

/* ---------------------------- FIXED TEAM DATA --------------------------- */
static const char *teamNames[NTEAMS] = {
    "KKR","GT","CSK","RCB","SRH","MI","DC","LSG","RR","PBKS"
};
static const char *venueNames[NTEAMS] = {
    "Eden Gardens", "Narendra Modi Stadium", "M.A. Chidambaram Stadium",
    "M. Chinnaswamy Stadium", "Rajiv Gandhi Int. Stadium", "Wankhede Stadium",
    "Arun Jaitley Stadium", "BRSABV Ekana Stadium", "Sawai Mansingh Stadium",
    "Maharaja Yadavindra Singh Stadium"
};
/* approximate lat/long of each home city, used for the travel optimizer */
static const double venueLat[NTEAMS] = {
    22.5645, 23.0913, 13.0627, 12.9789, 17.4065, 18.9389, 28.6358, 26.8467, 26.8988, 30.6810
};
static const double venueLon[NTEAMS] = {
    88.3433, 72.5972, 80.2792, 77.5993, 78.5504, 72.8258, 77.2431, 80.9462, 75.8047, 76.5762
};
/* popular teams get preference for weekend/holiday prime-time slots */
static const int popularIdx[] = {2, 3, 5};      /* CSK, RCB, MI */
static const int numPopular = 3;
/* rivalry pairs also get preference for weekend/holiday slots */
static const int rivalryPairs[][2] = { {5,2}, {3,2}, {0,5} }; /* MI-CSK, RCB-CSK, KKR-MI */
static const int numRivalry = 3;

/* ------------------------------- STATE ---------------------------------- */
typedef struct {
    int played, won, lost, points;
    double runsFor, oversFor, runsAgainst, oversAgainst;
    double nrr;
    int headToHead[NTEAMS];
} TeamStat;

TeamStat stats[NTEAMS];
int strength[NTEAMS];                       /* 1-100, default 50 */
char restrictedDates[NTEAMS][MAX_BLACKOUT][11];
int  restrictedCount[NTEAMS];
char holidayDates[MAX_BLACKOUT][11];
int  numHolidays = 0;

char startDateStr[11], endDateStr[11];
char fixTeam[16] = {0};
char fixVenue[100] = {0};
char seasonLabel[64] = {0};

/* ============================ CGI PARSING UTILS ========================= */
void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* find "key=" at a field boundary (start of body or right after '&') and
   return the url-decoded value; returns 1 if found, 0 otherwise */
int get_field(const char *body, const char *key, char *out, int outsz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    size_t klen = strlen(pattern);
    const char *p = body;
    while ((p = strstr(p, pattern)) != NULL) {
        if (p == body || *(p - 1) == '&') {
            const char *valStart = p + klen;
            const char *end = strchr(valStart, '&');
            size_t len = end ? (size_t)(end - valStart) : strlen(valStart);
            static char tmp[8192];
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, valStart, len);
            tmp[len] = '\0';
            url_decode(out, tmp);
            if ((int)strlen(out) >= outsz) out[outsz - 1] = '\0';
            return 1;
        }
        p += klen;
    }
    out[0] = '\0';
    return 0;
}

void trim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' ' || s[len-1] == '\t')) s[--len] = '\0';
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
}

/* split a block of text into trimmed lines (handles \n and \r\n) */
int split_lines(const char *text, char out[][LINE_BUF], int maxLines) {
    int n = 0;
    const char *p = text;
    char buf[LINE_BUF];
    while (*p && n < maxLines) {
        int i = 0;
        while (*p && *p != '\n' && i < LINE_BUF - 1) buf[i++] = *p++;
        buf[i] = '\0';
        if (*p == '\n') p++;
        trim(buf);
        strcpy(out[n], buf);
        n++;
    }
    return n;
}

/* split comma-separated dates into an array (caller-provided storage) */
int split_dates(const char *text, char out[][11], int maxN) {
    int n = 0;
    char tmp[512];
    strncpy(tmp, text, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *tok = strtok(tmp, ",");
    while (tok && n < maxN) {
        trim(tok);
        if (strlen(tok) > 0) { strncpy(out[n], tok, 10); out[n][10] = '\0'; n++; }
        tok = strtok(NULL, ",");
    }
    return n;
}

int teamIndex(const char *name) {
    for (int i = 0; i < NTEAMS; i++) if (strcmp(teamNames[i], name) == 0) return i;
    return -1;
}

int isBlackout(int teamIdx, const char *dateStr) {
    for (int k = 0; k < restrictedCount[teamIdx]; k++)
        if (strcmp(restrictedDates[teamIdx][k], dateStr) == 0) return 1;
    return 0;
}

int isHolidayDate(const char *dateStr) {
    for (int k = 0; k < numHolidays; k++)
        if (strcmp(holidayDates[k], dateStr) == 0) return 1;
    return 0;
}

int isPopular(int idx) {
    for (int k = 0; k < numPopular; k++) if (popularIdx[k] == idx) return 1;
    return 0;
}

int isRivalry(int a, int b) {
    for (int k = 0; k < numRivalry; k++) {
        if ((rivalryPairs[k][0] == a && rivalryPairs[k][1] == b) ||
            (rivalryPairs[k][0] == b && rivalryPairs[k][1] == a)) return 1;
    }
    return 0;
}

double haversine(double lat1, double lon1, double lat2, double lon2) {
    double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat/2)*sin(dLat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dLon/2)*sin(dLon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return R * c;
}

/* ============================== SCHEDULING =============================== */
void matches(void) {
    struct tm start_tm = {0}, end_tm = {0};
    int y, m, d;
    sscanf(startDateStr, "%d-%d-%d", &y, &m, &d);
    start_tm.tm_year = y - 1900; start_tm.tm_mon = m - 1; start_tm.tm_mday = d; start_tm.tm_hour = 12;
    sscanf(endDateStr, "%d-%d-%d", &y, &m, &d);
    end_tm.tm_year = y - 1900; end_tm.tm_mon = m - 1; end_tm.tm_mday = d; end_tm.tm_hour = 12;

    time_t cur = mktime(&start_tm);
    time_t end = mktime(&end_tm);

    FILE *fptr = fopen("Roundrobin.csv", "w");
    if (!fptr) { perror("Roundrobin.csv"); return; }
    fprintf(fptr, "Team1,Team2,Venue,Date,Time\n");

    printf("<h2>Match Schedule</h2><table>\n");
    printf("<tr><th>Team1</th><th>Team2</th><th>Venue</th><th>Date</th><th>Time</th></tr>\n");

    int matchScheduled[NTEAMS][NTEAMS] = {{0}};
    int lastPlayedDay[NTEAMS];
    int lastWasAway[NTEAMS] = {0};
    int lastVenue[NTEAMS];
    int homeStreak[NTEAMS] = {0};   /* consecutive home matches played in a row */
    int awayStreak[NTEAMS] = {0};   /* consecutive away matches played in a row */
    const int MAX_HOME_STREAK = 2;  /* at most 2 home matches back-to-back */
    const int MAX_AWAY_STREAK = 1;  /* no two away matches back-to-back at all */
    for (int i = 0; i < NTEAMS; i++) { lastPlayedDay[i] = -100; lastVenue[i] = i; }

    int total = NTEAMS * (NTEAMS - 1);   /* double round robin, directed pairs */
    int scheduled = 0;

    while (scheduled < total && cur <= end) {
        struct tm *tm_cur = localtime(&cur);
        int yday = tm_cur->tm_yday;
        int weekend = (tm_cur->tm_wday == 0 || tm_cur->tm_wday == 6);
        char dateStr[11];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tm_cur);
        int holiday = isHolidayDate(dateStr);

        int cap = weekend ? 2 : 1;          /* total matches for the WHOLE league today */
        int playedToday[NTEAMS] = {0};

        for (int slot = 0; slot < cap && scheduled < total; slot++) {
            int bestHost = -1, bestOpp = -1; double bestScore = 1e18;

            for (int host = 0; host < NTEAMS; host++) {
                if (playedToday[host] || isBlackout(host, dateStr)) continue;
                int required_host = lastWasAway[host] ? 3 : 2;
                int hostRested = (lastPlayedDay[host] < -50) || (yday - lastPlayedDay[host] >= required_host);
                if (!hostRested) continue;

                for (int j = 0; j < NTEAMS; j++) {
                    if (j == host || playedToday[j] || matchScheduled[host][j]) continue;
                    int required_j = lastWasAway[j] ? 3 : 2;
                    int jRested = (lastPlayedDay[j] < -50) || (yday - lastPlayedDay[j] >= required_j);
                    if (!jRested) continue;

                    double dist = haversine(venueLat[host], venueLon[host],
                                             venueLat[lastVenue[j]], venueLon[lastVenue[j]]);
                    double score = dist;
                    /* Balanced home/away streaks + no-consecutive-away are treated as strong
                       preferences (not hard blocks): a single-pass greedy scheduler can hit
                       genuine dead ends near the end of a double round-robin if these are
                       enforced as absolute rules (a team can run out of home fixtures to
                       "reset" an away streak with). Penalize heavily instead so the scheduler
                       avoids them whenever any alternative exists, without ever refusing to
                       schedule a fixture altogether. */
                    if (awayStreak[j] >= MAX_AWAY_STREAK) score += awayStreak[j] * 8000.0;
                    if (homeStreak[host] >= MAX_HOME_STREAK) score += homeStreak[host] * 2000.0;

                    /* prime evening slot (or the only weekday slot) favors marquee fixtures */
                    int isPrimeSlot = (slot == cap - 1);
                    if ((weekend || holiday) && isPrimeSlot &&
                        (isRivalry(host, j) || isPopular(host) || isPopular(j)))
                        score -= 5000.0;

                    if (score < bestScore) { bestScore = score; bestHost = host; bestOpp = j; }
                }
            }
            if (bestHost == -1) break; /* nothing schedulable in this slot today */

            const char *slotTime = (weekend && slot == 0) ? "15:30" : "19:30";
            printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n",
                   teamNames[bestHost], teamNames[bestOpp], venueNames[bestHost], dateStr, slotTime);
            fprintf(fptr, "%s,%s,%s,%s,%s\n",
                    teamNames[bestHost], teamNames[bestOpp], venueNames[bestHost], dateStr, slotTime);

            matchScheduled[bestHost][bestOpp] = 1;
            lastPlayedDay[bestHost] = yday; lastPlayedDay[bestOpp] = yday;
            lastWasAway[bestHost] = 0; lastWasAway[bestOpp] = 1;
            lastVenue[bestHost] = bestHost; lastVenue[bestOpp] = bestHost;
            homeStreak[bestHost]++;  awayStreak[bestHost] = 0;
            awayStreak[bestOpp]++;   homeStreak[bestOpp] = 0;
            playedToday[bestHost] = 1; playedToday[bestOpp] = 1;
            scheduled++;
        }
        cur += 24 * 60 * 60;
    }
    printf("</table>\n");
    if (scheduled < total)
        printf("<p style='color:#ffcc66'><b>Warning:</b> only %d of %d league fixtures could be "
               "scheduled in the given date range/blackout constraints.</p>\n", scheduled, total);
    fclose(fptr);
}

/* ============================ STADIUM VIEW =============================== */
void stadium(void) {
    if (strlen(fixVenue) == 0) return;
    FILE *fptr = fopen("Roundrobin.csv", "r");
    if (!fptr) return;
    char line[200];
    printf("<h2>Stadium Specific Fixtures: %s</h2><table>\n", fixVenue);
    printf("<tr><th>Team1</th><th>Team2</th><th>Venue</th><th>Date</th><th>Time</th></tr>\n");
    fgets(line, sizeof(line), fptr);
    while (fgets(line, sizeof(line), fptr)) {
        char t1[10], t2[10], venue[100], date[20], time_[10];
        if (sscanf(line, "%9[^,],%9[^,],%99[^,],%19[^,],%9[^\n]", t1, t2, venue, date, time_) == 5) {
            if (strcmp(venue, fixVenue) == 0)
                printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n", t1, t2, venue, date, time_);
        }
    }
    printf("</table>\n");
    fclose(fptr);
}

/* ============================ TEAM FIXTURES =============================== */
void fixtures(void) {
    if (strlen(fixTeam) == 0) return;
    FILE *fptr = fopen("Roundrobin.csv", "r");
    if (!fptr) return;
    char line[200];
    printf("<h2>Team Specific Fixtures: %s</h2><table>\n", fixTeam);
    printf("<tr><th>Team1</th><th>Team2</th><th>Venue</th><th>Date</th><th>Time</th></tr>\n");
    fgets(line, sizeof(line), fptr);
    while (fgets(line, sizeof(line), fptr)) {
        char t1[10], t2[10], venue[100], date[20], time_[10];
        if (sscanf(line, "%9[^,],%9[^,],%99[^,],%19[^,],%9[^\n]", t1, t2, venue, date, time_) == 5) {
            if (strcmp(fixTeam, t1) == 0 || strcmp(fixTeam, t2) == 0)
                printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n", t1, t2, venue, date, time_);
        }
    }
    printf("</table>\n");
    fclose(fptr);
}

/* ================== RESULTS (manual and/or simulated) + POINTS =========== */
void results_and_points(const char *resultsText) {
    char resLines[MAX_RESULTS][LINE_BUF];
    int nRes = split_lines(resultsText, resLines, MAX_RESULTS);

    for (int i = 0; i < NTEAMS; i++) memset(&stats[i], 0, sizeof(TeamStat));

    FILE *fptr = fopen("Roundrobin.csv", "r");
    if (!fptr) { printf("<p>No schedule found - generate the schedule first.</p>\n"); return; }
    FILE *fout = fopen("MatchResults.csv", "w");
    fprintf(fout, "Team1,Team2,Venue,Date,Time,Runs1,Runs2,Winner,PlayerOfMatch\n");

    char line[200];
    fgets(line, sizeof(line), fptr); /* header */
    int idx = 0;
    printf("<h2>Match Results</h2><table>\n");
    printf("<tr><th>Team1</th><th>Team2</th><th>Date</th><th>Score1</th><th>Score2</th><th>Winner</th><th>POTM</th></tr>\n");

    while (fgets(line, sizeof(line), fptr)) {
        char t1[10], t2[10], venue[100], date[20], time_[10];
        if (sscanf(line, "%9[^,],%9[^,],%99[^,],%19[^,],%9[^\n]", t1, t2, venue, date, time_) != 5) continue;
        int i = teamIndex(t1), j = teamIndex(t2);
        if (i < 0 || j < 0) continue;

        int r1 = 0, r2 = 0; char potm[24] = "";
        int manual = 0;
        if (idx < nRes && strlen(resLines[idx]) > 0) {
            int got = sscanf(resLines[idx], "%d,%d,%23[^\n]", &r1, &r2, potm);
            if (got >= 2) manual = 1;
        }
        if (!manual) {
            double p = (double) strength[i] / (double)(strength[i] + strength[j]);
            double r = (double) rand() / RAND_MAX;
            int base = 140 + rand() % 60, margin = 5 + rand() % 30;
            if (r < p) { r1 = base + margin; r2 = base; } else { r2 = base + margin; r1 = base; }
            strcpy(potm, "-");
        }
        int winner = (r1 == r2) ? ((rand() % 2) ? i : j) : (r1 > r2 ? i : j);

        stats[i].played++; stats[j].played++;
        stats[i].runsFor += r1; stats[i].oversFor += 20; stats[i].runsAgainst += r2; stats[i].oversAgainst += 20;
        stats[j].runsFor += r2; stats[j].oversFor += 20; stats[j].runsAgainst += r1; stats[j].oversAgainst += 20;

        if (winner == i) { stats[i].won++; stats[i].points += 2; stats[j].lost++; stats[i].headToHead[j]++; }
        else              { stats[j].won++; stats[j].points += 2; stats[i].lost++; stats[j].headToHead[i]++; }

        stats[i].nrr = (stats[i].runsFor / stats[i].oversFor) - (stats[i].runsAgainst / stats[i].oversAgainst);
        stats[j].nrr = (stats[j].runsFor / stats[j].oversFor) - (stats[j].runsAgainst / stats[j].oversAgainst);

        printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td><b>%s</b></td><td>%s</td></tr>\n",
               t1, t2, date, r1, r2, teamNames[winner], potm);
        fprintf(fout, "%s,%s,%s,%s,%s,%d,%d,%s,%s\n", t1, t2, venue, date, time_, r1, r2, teamNames[winner], potm);
        idx++;
    }
    printf("</table>\n");
    fclose(fptr); fclose(fout);

    /* sort: points desc -> NRR desc -> head-to-head desc */
    int order[NTEAMS];
    for (int i = 0; i < NTEAMS; i++) order[i] = i;
    for (int a = 0; a < NTEAMS - 1; a++)
        for (int b = 0; b < NTEAMS - 1 - a; b++) {
            int x = order[b], y = order[b + 1];
            int swap = 0;
            if (stats[x].points < stats[y].points) swap = 1;
            else if (stats[x].points == stats[y].points && stats[x].nrr < stats[y].nrr) swap = 1;
            else if (stats[x].points == stats[y].points && stats[x].nrr == stats[y].nrr &&
                     stats[x].headToHead[y] < stats[y].headToHead[x]) swap = 1;
            if (swap) { order[b] = y; order[b + 1] = x; }
        }

    FILE *fp1 = fopen("file1.csv", "w");
    fprintf(fp1, "Position,Team Name,Score,Win,Lose,NRR\n");
    printf("<h2>Points Table</h2><table>\n");
    printf("<tr><th>POSITION</th><th>TEAM</th><th>POINTS</th><th>WIN</th><th>LOSE</th><th>NRR</th></tr>\n");
    for (int p = 0; p < NTEAMS; p++) {
        int t = order[p];
        printf("<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%.3f</td></tr>\n",
               p + 1, teamNames[t], stats[t].points, stats[t].won, stats[t].lost, stats[t].nrr);
        fprintf(fp1, "%d,%s,%d,%d,%d,%.3f\n", p + 1, teamNames[t], stats[t].points, stats[t].won, stats[t].lost, stats[t].nrr);
    }
    printf("</table>\n");
    fclose(fp1);
}

/* ============================ SETTINGS PERSISTENCE ========================= */
/* Season label + strength ratings are entered once on the setup page, then
   saved here so the later step pages (fixtures/stadium/results/playoffs)
   don't need to ask for them again. */
void save_settings(void) {
    FILE *f = fopen("settings.csv", "w");
    if (!f) return;
    fprintf(f, "Season,%s\n", strlen(seasonLabel) ? seasonLabel : "Untitled Season");
    for (int i = 0; i < NTEAMS; i++) fprintf(f, "Strength,%s,%d\n", teamNames[i], strength[i]);
    fclose(f);
}

void load_settings(void) {
    for (int i = 0; i < NTEAMS; i++) strength[i] = 50;
    FILE *f = fopen("settings.csv", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Season,", 7) == 0) {
            char val[64]; strncpy(val, line + 7, sizeof(val) - 1); val[sizeof(val)-1] = '\0';
            trim(val);
            strncpy(seasonLabel, val, sizeof(seasonLabel) - 1);
        } else if (strncmp(line, "Strength,", 9) == 0) {
            char team[10]; int val;
            if (sscanf(line + 9, "%9[^,],%d", team, &val) == 2) {
                int idx = teamIndex(team);
                if (idx >= 0) strength[idx] = val;
            }
        }
    }
    fclose(f);
}

/* ================================ NAVIGATION =============================== */
void archive_current_season(int seasonNum);   /* forward declaration — defined below main() ordering */
int count_history_seasons(void);

void print_nav(void) {
    printf("<div style='text-align:center;margin:25px 0;padding:15px;'>\n");
    printf("<a class='nav' href='fixtures.html'>Team Fixtures</a>"
           "<a class='nav' href='stadium.html'>Stadium Fixtures</a>"
           "<a class='nav' href='results.html'>Enter / Simulate Results</a>"
           "<a class='nav' href='playoffs.html'>Run Playoffs</a>"
           "<a class='nav' href='history.html'>All-Time Champions</a>"
           "<a class='nav' href='index.html'>Start a New Season</a>\n");
    printf("</div>\n");
}

/* ================================ PLAYOFFS ================================ */
/* returns winner name into 'out'; if manualLine names one of a/b use that,
   else decide via strength-weighted coin flip */
void decideWinner(const char *a, const char *b, const char *manualLine, char *out) {
    char trimmed[32];
    strncpy(trimmed, manualLine, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    trim(trimmed);
    if (strcmp(trimmed, a) == 0) { strcpy(out, a); return; }
    if (strcmp(trimmed, b) == 0) { strcpy(out, b); return; }

    int ia = teamIndex(a), ib = teamIndex(b);
    int sa = (ia >= 0) ? strength[ia] : 50, sb = (ib >= 0) ? strength[ib] : 50;
    double p = (double) sa / (double)(sa + sb);
    double r = (double) rand() / RAND_MAX;
    strcpy(out, (r < p) ? a : b);
}

void print_history_tally(void) {
    FILE *h = fopen("history.csv", "r");
    if (!h) { printf("<p>No season history recorded yet.</p>\n"); return; }
    int idxOf[64]; char names[64][24]; int cnt[64]; int nUnique = 0;
    int nEntries = 0;
    char line[128];

    printf("<h2>Season History</h2><table>\n");
    printf("<tr><th>#</th><th>Season</th><th>Champion</th><th>Runner-up</th></tr>\n");
    while (fgets(line, sizeof(line), h)) {
        char season[64], champ[24], runner[24];
        if (sscanf(line, "%63[^,],%23[^,],%23[^\n]", season, champ, runner) == 3) {
            nEntries++;
            printf("<tr><td>%d</td><td>%s</td><td><b>%s</b></td><td>%s</td></tr>\n", nEntries, season, champ, runner);
            int found = -1;
            for (int k = 0; k < nUnique; k++) if (strcmp(names[k], champ) == 0) found = k;
            if (found == -1) { strcpy(names[nUnique], champ); cnt[nUnique] = 1; nUnique++; }
            else cnt[found]++;
        }
    }
    fclose(h);
    printf("</table>\n");
    (void)idxOf;

    if (nEntries == 0) { printf("<p>No season history recorded yet.</p>\n"); return; }

    printf("<h2>All-Time Champions (%d season%s recorded)</h2><table>\n", nEntries, nEntries == 1 ? "" : "s");
    printf("<tr><th>Team</th><th>Titles</th></tr>\n");
    /* simple sort by count desc */
    for (int a = 0; a < nUnique - 1; a++)
        for (int b = 0; b < nUnique - 1 - a; b++)
            if (cnt[b] < cnt[b+1]) { int tc = cnt[b]; cnt[b]=cnt[b+1]; cnt[b+1]=tc;
                                      char tn[24]; strcpy(tn, names[b]); strcpy(names[b], names[b+1]); strcpy(names[b+1], tn); }
    for (int k = 0; k < nUnique; k++)
        printf("<tr><td>%s</td><td>%d</td></tr>\n", names[k], cnt[k]);
    printf("</table>\n");
}

void playoff(char playoffLines[4][LINE_BUF]) {
    FILE *fptr = fopen("file1.csv", "r");
    if (!fptr) { printf("<p>No points table found - generate the schedule and results first.</p>\n"); return; }
    char line[128];
    fgets(line, sizeof(line), fptr); /* header */

    char nm[4][24]; int pts[4], w[4], l[4]; float nr[4];
    int got4 = 0;
    for (int k = 0; k < 4; k++) {
        if (!fgets(line, sizeof(line), fptr)) break;
        if (sscanf(line, "%*d,%23[^,],%d,%d,%d,%f", nm[k], &pts[k], &w[k], &l[k], &nr[k]) == 5) got4++;
    }
    fclose(fptr);
    if (got4 < 4) { printf("<p>Not enough teams in the points table to run playoffs.</p>\n"); return; }

    printf("<h2>Top 4</h2><table><tr><th>Pos</th><th>Team</th><th>Points</th><th>Win</th><th>Lose</th><th>NRR</th></tr>\n");
    for (int k = 0; k < 4; k++)
        printf("<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%.3f</td></tr>\n",
               k + 1, nm[k], pts[k], w[k], l[k], nr[k]);
    printf("</table>\n");

    char q1w[24], q1l[24], ew[24], el[24], q2w[24], champ[24];

    decideWinner(nm[0], nm[1], playoffLines[0], q1w);
    strcpy(q1l, (strcmp(q1w, nm[0]) == 0) ? nm[1] : nm[0]);
    printf("<h2>Qualifier 1</h2><p>%s vs %s &rarr; Winner: <b>%s</b> (advances to Final). "
           "%s gets a second chance in Qualifier 2.</p>\n", nm[0], nm[1], q1w, q1l);

    decideWinner(nm[2], nm[3], playoffLines[1], ew);
    strcpy(el, (strcmp(ew, nm[2]) == 0) ? nm[3] : nm[2]);
    printf("<h2>Eliminator</h2><p>%s vs %s &rarr; Winner: <b>%s</b> (advances to Qualifier 2). "
           "%s is eliminated.</p>\n", nm[2], nm[3], ew, el);

    decideWinner(q1l, ew, playoffLines[2], q2w);
    printf("<h2>Qualifier 2</h2><p>%s vs %s &rarr; Winner: <b>%s</b> (advances to Final).</p>\n", q1l, ew, q2w);

    decideWinner(q1w, q2w, playoffLines[3], champ);
    char runnerUp[24];
    strcpy(runnerUp, (strcmp(champ, q1w) == 0) ? q2w : q1w);
    printf("<h2>FINAL</h2><p>%s vs %s</p>\n", q1w, q2w);
    printf("<h1 style='color:gold'>&#127942; IPL CHAMPION: %s &#127942;</h1>\n", champ);

    FILE *pf = fopen("PlayoffResults.csv", "w");
    if (pf) {
        fprintf(pf, "Stage,Team1,Team2,Winner\n");
        fprintf(pf, "Qualifier 1,%s,%s,%s\n", nm[0], nm[1], q1w);
        fprintf(pf, "Eliminator,%s,%s,%s\n", nm[2], nm[3], ew);
        fprintf(pf, "Qualifier 2,%s,%s,%s\n", q1l, ew, q2w);
        fprintf(pf, "Final,%s,%s,%s\n", q1w, q2w, champ);
        fclose(pf);
    }

    FILE *h = fopen("history.csv", "a");
    if (h) {
        fprintf(h, "%s,%s,%s\n", strlen(seasonLabel) ? seasonLabel : "Untitled Season", champ, runnerUp);
        fclose(h);
    }
    int seasonNum = count_history_seasons();
    archive_current_season(seasonNum);
    printf("<p style='color:#9fffb0'>This result has been added to season history "
           "(Season #%d). See the <a class='nav' style='display:inline' "
           "href='history.html'>All-Time Champions</a> page for the full record.</p>\n", seasonNum);

    FILE *rep = fopen("season_report.txt", "w");
    if (rep) {
        fprintf(rep, "IPL SEASON REPORT: %s\n", strlen(seasonLabel) ? seasonLabel : "Untitled Season");
        fprintf(rep, "=====================================\n\n");
        fprintf(rep, "POINTS TABLE (see file1.csv for full data)\n");
        fprintf(rep, "Top 4: %s, %s, %s, %s\n\n", nm[0], nm[1], nm[2], nm[3]);
        fprintf(rep, "Qualifier 1: %s vs %s -> %s\n", nm[0], nm[1], q1w);
        fprintf(rep, "Eliminator : %s vs %s -> %s\n", nm[2], nm[3], ew);
        fprintf(rep, "Qualifier 2: %s vs %s -> %s\n", q1l, ew, q2w);
        fprintf(rep, "FINAL      : %s vs %s -> CHAMPION: %s\n", q1w, q2w, champ);
        fclose(rep);
    }
    printf("<p><i>A plain-text season_report.txt has also been saved on the server.</i></p>\n");
}

/* ============================ SEASON ARCHIVING ============================ */
void copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return; }
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
}

int count_history_seasons(void) {
    FILE *h = fopen("history.csv", "r");
    if (!h) return 0;
    int n = 0; char line[128];
    while (fgets(line, sizeof(line), h)) {
        char season[64], champ[24], runner[24];
        if (sscanf(line, "%63[^,],%23[^,],%23[^\n]", season, champ, runner) == 3) n++;
    }
    fclose(h);
    return n;
}

void archive_current_season(int seasonNum) {
    char path[64];
    snprintf(path, sizeof(path), "archive_season_%d_schedule.csv", seasonNum);
    copy_file("Roundrobin.csv", path);
    snprintf(path, sizeof(path), "archive_season_%d_results.csv", seasonNum);
    copy_file("MatchResults.csv", path);
    snprintf(path, sizeof(path), "archive_season_%d_points.csv", seasonNum);
    copy_file("file1.csv", path);
    snprintf(path, sizeof(path), "archive_season_%d_playoffs.csv", seasonNum);
    copy_file("PlayoffResults.csv", path);
}

/* Displays one past season in full: champion/runner-up, schedule, match
   results, and final points table — read back from the archived CSVs.
   Older history.csv rows created before archiving existed simply won't
   have archive files, so each section says so gracefully instead of
   printing nothing. */
void view_archived_season(int n) {
    if (n <= 0) { printf("<p>Please enter a valid season number (see the table above).</p>\n"); return; }

    FILE *h = fopen("history.csv", "r");
    char season[64] = "", champ[24] = "", runner[24] = "";
    int found = 0;
    if (h) {
        char line[128]; int idx = 0;
        while (fgets(line, sizeof(line), h)) {
            char s[64], c[24], r[24];
            if (sscanf(line, "%63[^,],%23[^,],%23[^\n]", s, c, r) == 3) {
                idx++;
                if (idx == n) { strcpy(season, s); strcpy(champ, c); strcpy(runner, r); found = 1; break; }
            }
        }
        fclose(h);
    }
    if (!found) { printf("<p>Season %d not found in history.csv.</p>\n", n); return; }

    printf("<h2>%s</h2><p>Champion: <b>%s</b> &nbsp;&nbsp; Runner-up: %s</p>\n", season, champ, runner);

    char path[64]; FILE *f; char line[220];

    snprintf(path, sizeof(path), "archive_season_%d_schedule.csv", n);
    f = fopen(path, "r");
    if (f) {
        printf("<h3>Schedule</h3><table><tr><th>Team1</th><th>Team2</th><th>Venue</th><th>Date</th><th>Time</th></tr>\n");
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            char t1[10], t2[10], venue[100], date[20], time_[10];
            if (sscanf(line, "%9[^,],%9[^,],%99[^,],%19[^,],%9[^\n]", t1, t2, venue, date, time_) == 5)
                printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n", t1, t2, venue, date, time_);
        }
        printf("</table>\n");
        fclose(f);
    } else {
        printf("<p><i>No archived schedule found for this season (it may predate detailed archiving).</i></p>\n");
    }

    snprintf(path, sizeof(path), "archive_season_%d_results.csv", n);
    f = fopen(path, "r");
    if (f) {
        printf("<h3>Match Results</h3><table><tr><th>Team1</th><th>Team2</th><th>Date</th><th>Score1</th><th>Score2</th><th>Winner</th><th>POTM</th></tr>\n");
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            char t1[10], t2[10], venue[100], date[20], time_[10], winner[10], potm[24]; int r1, r2;
            if (sscanf(line, "%9[^,],%9[^,],%99[^,],%19[^,],%9[^,],%d,%d,%9[^,],%23[^\n]",
                       t1, t2, venue, date, time_, &r1, &r2, winner, potm) == 9)
                printf("<tr><td>%s</td><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td><b>%s</b></td><td>%s</td></tr>\n",
                       t1, t2, date, r1, r2, winner, potm);
        }
        printf("</table>\n");
        fclose(f);
    } else {
        printf("<p><i>No archived match results found for this season.</i></p>\n");
    }

    snprintf(path, sizeof(path), "archive_season_%d_points.csv", n);
    f = fopen(path, "r");
    if (f) {
        printf("<h3>Final Points Table</h3><table><tr><th>Position</th><th>Team</th><th>Points</th><th>Win</th><th>Lose</th><th>NRR</th></tr>\n");
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            int pos, pts, w, l; char team[24]; float nrr;
            if (sscanf(line, "%d,%23[^,],%d,%d,%d,%f", &pos, team, &pts, &w, &l, &nrr) == 6)
                printf("<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%.3f</td></tr>\n",
                       pos, team, pts, w, l, nrr);
        }
        printf("</table>\n");
        fclose(f);
    } else {
        printf("<p><i>No archived points table found for this season.</i></p>\n");
    }

    snprintf(path, sizeof(path), "archive_season_%d_playoffs.csv", n);
    f = fopen(path, "r");
    if (f) {
        printf("<h3>Playoffs</h3><table><tr><th>Stage</th><th>Team 1</th><th>Team 2</th><th>Winner</th></tr>\n");
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            char stage[24], t1[24], t2[24], winner[24];
            if (sscanf(line, "%23[^,],%23[^,],%23[^,],%23[^\n]", stage, t1, t2, winner) == 4)
                printf("<tr><td>%s</td><td>%s</td><td>%s</td><td><b>%s</b></td></tr>\n", stage, t1, t2, winner);
        }
        printf("</table>\n");
        fclose(f);
    } else {
        printf("<p><i>No archived playoff bracket found for this season.</i></p>\n");
    }
}

/* ================================== MAIN ================================== */
int main(void) {
    printf("Content-Type: text/html\n\n");
    printf("<html><head><style>\n"
           "body{background: linear-gradient(to right, #000c66, #001179);font-family:Arial,sans-serif;}\n"
           "h1,h2,h3{text-shadow:2px 2px 10px red;color:#ffff66;text-align:center;}\n"
           "table{margin:20px auto;border-collapse:collapse;width:80%%;}\n"
           "td{border-bottom:1px solid white;padding:8px;color:white;text-align:center;}\n"
           "th{background:linear-gradient(45deg,#f9d423,#ff4e50);padding:8px;}\n"
           "tr:hover{background-color:slateblue;}\n"
           "p{color:#eee;text-align:center;}\n"
           "a.nav{display:inline-block;margin:4px;padding:8px 14px;border-radius:20px;"
           "background:linear-gradient(45deg,#f9d423,#ff4e50);color:#001179;font-weight:bold;"
           "text-decoration:none;}\n"
           "a.nav:hover{transform:scale(1.05);}\n"
           "</style></head><body>\n");
    printf("<h1>&#127942; IPL Schedule Generator &#127942;</h1>\n");

    char *lenStr = getenv("CONTENT_LENGTH");
    int len = lenStr ? atoi(lenStr) : 0;
    char *input = malloc(len + 1);
    if (len > 0) fread(input, 1, len, stdin);
    input[len] = '\0';

    char action[16] = "schedule";
    get_field(input, "action", action, sizeof(action));
    srand((unsigned int) time(NULL));

    if (strcmp(action, "schedule") == 0) {
        /* --- SETUP PAGE: generate the season schedule and persist settings --- */
        get_field(input, "start", startDateStr, sizeof(startDateStr));
        get_field(input, "end", endDateStr, sizeof(endDateStr));
        get_field(input, "season", seasonLabel, sizeof(seasonLabel));

        char holidayRaw[512] = "";
        get_field(input, "holidays", holidayRaw, sizeof(holidayRaw));
        numHolidays = split_dates(holidayRaw, holidayDates, MAX_BLACKOUT);

        for (int i = 0; i < NTEAMS; i++) {
            char raw[512] = "";
            get_field(input, teamNames[i], raw, sizeof(raw));
            restrictedCount[i] = split_dates(raw, restrictedDates[i], MAX_BLACKOUT);

            char skey[24], sval[8] = "";
            snprintf(skey, sizeof(skey), "strength_%s", teamNames[i]);
            get_field(input, skey, sval, sizeof(sval));
            int s = atoi(sval);
            strength[i] = (s >= 1 && s <= 100) ? s : 50;
        }

        if (strlen(startDateStr) == 0 || strlen(endDateStr) == 0) {
            printf("<p style='color:#ff6666'>Please provide both a start and end date.</p>\n");
        } else {
            matches();
            save_settings();
            printf("<p style='color:#9fffb0'>Schedule generated and saved. Use the menu below "
                   "to look up fixtures, enter results, or run the playoffs whenever you're ready.</p>\n");
        }

    } else if (strcmp(action, "fixtures") == 0) {
        load_settings();
        get_field(input, "fix", fixTeam, sizeof(fixTeam));
        fixtures();

    } else if (strcmp(action, "stadium") == 0) {
        load_settings();
        get_field(input, "venue", fixVenue, sizeof(fixVenue));
        stadium();

    } else if (strcmp(action, "results") == 0) {
        load_settings();
        static char resultsRaw[8192] = "";
        get_field(input, "results", resultsRaw, sizeof(resultsRaw));
        results_and_points(resultsRaw);

    } else if (strcmp(action, "playoffs") == 0) {
        load_settings();
        char playoffLines[4][LINE_BUF];
        get_field(input, "playoff1", playoffLines[0], LINE_BUF);
        get_field(input, "playoff2", playoffLines[1], LINE_BUF);
        get_field(input, "playoff3", playoffLines[2], LINE_BUF);
        get_field(input, "playoff4", playoffLines[3], LINE_BUF);
        playoff(playoffLines);

    } else if (strcmp(action, "history") == 0) {
        print_history_tally();

    } else if (strcmp(action, "view_season") == 0) {
        char snum[8] = "";
        get_field(input, "season_num", snum, sizeof(snum));
        view_archived_season(atoi(snum));

    } else if (strcmp(action, "full") == 0) {
        /* legacy one-shot path: everything in a single request */
        get_field(input, "start", startDateStr, sizeof(startDateStr));
        get_field(input, "end", endDateStr, sizeof(endDateStr));
        get_field(input, "season", seasonLabel, sizeof(seasonLabel));
        char holidayRaw[512] = "";
        get_field(input, "holidays", holidayRaw, sizeof(holidayRaw));
        numHolidays = split_dates(holidayRaw, holidayDates, MAX_BLACKOUT);
        for (int i = 0; i < NTEAMS; i++) {
            char raw[512] = "";
            get_field(input, teamNames[i], raw, sizeof(raw));
            restrictedCount[i] = split_dates(raw, restrictedDates[i], MAX_BLACKOUT);
            char skey[24], sval[8] = "";
            snprintf(skey, sizeof(skey), "strength_%s", teamNames[i]);
            get_field(input, skey, sval, sizeof(sval));
            int s = atoi(sval);
            strength[i] = (s >= 1 && s <= 100) ? s : 50;
        }
        static char resultsRaw2[8192] = "";
        get_field(input, "results", resultsRaw2, sizeof(resultsRaw2));
        char playoffLines[4][LINE_BUF];
        get_field(input, "playoff1", playoffLines[0], LINE_BUF);
        get_field(input, "playoff2", playoffLines[1], LINE_BUF);
        get_field(input, "playoff3", playoffLines[2], LINE_BUF);
        get_field(input, "playoff4", playoffLines[3], LINE_BUF);
        if (strlen(startDateStr) == 0 || strlen(endDateStr) == 0) {
            printf("<p style='color:#ff6666'>Please provide both a start and end date.</p>\n");
        } else {
            matches(); save_settings();
            results_and_points(resultsRaw2);
            playoff(playoffLines);
        }
    }

    print_nav();
    printf("</body></html>\n");
    free(input);
    return 0;
}