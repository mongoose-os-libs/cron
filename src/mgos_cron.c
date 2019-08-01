/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <time.h>

#include "ccronexpr.h"
#include "sunriset.h"

#include "common/cs_dbg.h"
#include "common/queue.h"
#include "common/str_util.h"

#include "mgos_cron.h"
#include "mgos_event.h"
#include "mgos_hal.h"
#include "mgos_location.h"
#include "mgos_sys_config.h"
#include "mgos_timers.h"
#include "mgos_utils.h"

#include "frozen.h"
#include "mongoose.h"

/* Max offset to be used with @sunrise and @sunset : 12 hours */
#define MAX_SUNRISE_OFFSET_SEC (12 * 60 * 60)

#define ONE_DAY (24 * 60 * 60)
#define SUNRISE_SPEC "@sunrise"
#define SUNSET_SPEC "@sunset"

#define MAX_FREE_JOBS_CNT 3

enum mgos_cron_expr_type {
  MGOS_CRON_EXPR_TYPE_NONE,
  MGOS_CRON_EXPR_TYPE_CCRONEXPR,
  MGOS_CRON_EXPR_TYPE_RANDOM,
  MGOS_CRON_EXPR_TYPE_SUN,
};

enum mgos_cron_sun {
  MGOS_CRON_SUNRISE,
  MGOS_CRON_SUNSET,
};

/*
 * Cron expression, exact type of which is determined by the `type` field:
 * so far it's only a standard expression which can be parsed by ccronexpr,
 * TODO(dfrank): add more types: Sun and Random.
 */
struct mgos_cron_expr {
  enum mgos_cron_expr_type type;
  union {
    cron_expr *ccronexpr;

    struct {
      cron_expr *from;
      cron_expr *to;
      int number;
    } random;

    struct {
      enum mgos_cron_sun type;
      int offset;
      cron_expr *daybegin;
    } sun;

  } data;
};

struct mgos_cron_entry {
  struct mgos_cron_expr expr;
  mgos_timer_id timer_id;
  mgos_cron_callback_t cb;
  time_t planned_time;
  void *user_data;
  SLIST_ENTRY(mgos_cron_entry) entries;
};

SLIST_HEAD(s_cron_entries, mgos_cron_entry)
s_cron_entries = SLIST_HEAD_INITIALIZER(s_cron_entries);

static int s_cron_entries_cnt = 0;

static bool s_cron_add(struct mgos_cron_entry *ce) {
  bool ret = true;
  mgos_lock();

#if defined(MGOS_FREE_BUILD)
  if (s_cron_entries_cnt >= MAX_FREE_JOBS_CNT) {
    LOG(LL_ERROR, ("Free version of cron library can only have %d jobs max. "
                   "For commercial version, please contact "
                   "https://mongoose-os.com/contact.html",
                   MAX_FREE_JOBS_CNT));
    goto clean;
  }
#endif

  SLIST_INSERT_HEAD(&s_cron_entries, ce, entries);
  s_cron_entries_cnt++;
  goto clean;

clean:
  mgos_unlock();
  return ret;
}

static void s_cron_entry_free(struct mgos_cron_entry *ce) {
  switch (ce->expr.type) {
    case MGOS_CRON_EXPR_TYPE_NONE:
      /* Nothing to do */
      return;

    case MGOS_CRON_EXPR_TYPE_CCRONEXPR:
      cron_expr_free(ce->expr.data.ccronexpr);
      ce->expr.data.ccronexpr = NULL;
      ce->expr.type = MGOS_CRON_EXPR_TYPE_NONE;
      return;

    case MGOS_CRON_EXPR_TYPE_RANDOM:
      cron_expr_free(ce->expr.data.random.from);
      ce->expr.data.random.from = NULL;
      cron_expr_free(ce->expr.data.random.to);
      ce->expr.data.random.to = NULL;
      ce->expr.type = MGOS_CRON_EXPR_TYPE_NONE;
      return;

    case MGOS_CRON_EXPR_TYPE_SUN:
      cron_expr_free(ce->expr.data.sun.daybegin);
      ce->expr.data.sun.daybegin = NULL;
      ce->expr.type = MGOS_CRON_EXPR_TYPE_NONE;
      return;
  }

  LOG(LL_ERROR, ("invalid expr type: %d", ce->expr.type));
  abort();
}

static void s_cron_remove(struct mgos_cron_entry *ce) {
  struct mgos_cron_entry *p;
  mgos_lock();
  SLIST_FOREACH(p, &s_cron_entries, entries) {
    if (p == ce) break;
  }
  if (p == NULL) goto clean;
  SLIST_REMOVE(&s_cron_entries, ce, mgos_cron_entry, entries);
  s_cron_entries_cnt--;
  mgos_clear_timer(ce->timer_id);
  s_cron_entry_free(ce);
  free(ce);
clean:
  mgos_unlock();
}

static void s_cron_timer_cb(void *arg);

/*
 * Distributes the `number` points between `from` and `to` evenly, and returns
 * the earliest one in between of `now` and `to`. `from` must be earlier than
 * `to`; `now` can be any value (but it probably makes little sense to use
 * `now` which is later than `to`).
 *
 * If we got no points in between of `now` and `to`, ((time_t) -1) is returned.
 */
static time_t s_next_random(time_t from, time_t to, time_t now, int number) {
  assert(to > from);
  time_t diff = to - from;
  int i;

  time_t ret = (time_t) -1;
  for (i = 0; i < number; i++) {
    time_t cur = from + (time_t) mgos_rand_range(0.0, (float) diff);
    if (cur > now && (ret == ((time_t) -1) || cur < ret)) {
      ret = cur;
    }
  }
  return ret;
}

/*
 * Resets hours, minutess and seconds of the given `date` to zeros, then
 * applies given offset (expressed in floating-point number of hours) and
 * returns the result. Offset can be negative.
 */
static time_t s_apply_hours_offset(time_t date, double offset_hours) {
  /* Rewind date to the beginning of the day */
  {
    struct tm t;
#ifdef _REENT
    gmtime_r(&date, &t);
#else
    memcpy(&t, gmtime(&date), sizeof(t));
#endif
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    date = (time_t) cs_timegm(&t);
  }

  /* Apply provided offset */
  int offset_seconds = (int) (offset_hours * 60.0 * 60.0);
  date += offset_seconds;

  return date;
}

static void s_print_time(enum cs_log_level level, const char *str,
                         time_t date) {
  struct tm t;
#ifdef _REENT
  gmtime_r(&date, &t);
#else
  memcpy(&t, gmtime(&date), sizeof(t));
#endif
  LOG(level, ("%s: %d [%04d/%02d/%02d %02d:%02d:%02d UTC]", str, (int) date,
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
              t.tm_sec));
}

/*
 * Returns time of sunrise or sunset (which of the two is determined by the
 * given `type`) at the given date (hours, minutes and seconds are ignored) for
 * the given lat/lon. The returned time might actually be on another day.
 */
static time_t s_get_sun_time2(time_t date, enum mgos_cron_sun type, double lat,
                              double lon) {
  double sunrise = 0;
  double sunset = 0;

  struct tm t;

#ifdef _REENT
  gmtime_r(&date, &t);
#else
  memcpy(&t, gmtime(&date), sizeof(t));
#endif
  sun_rise_set(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, lon, lat, &sunrise,
               &sunset);
  return s_apply_hours_offset(date,
                              type == MGOS_CRON_SUNRISE ? sunrise : sunset);
}

/*
 * Returns the sunrise or sunset time (depending on ce->expr.data.sun.type),
 * next after the given `date`, for the given lat/lon.
 */
static time_t s_get_sun_time(struct mgos_cron_entry *ce, time_t date,
                             double lat, double lon) {
  time_t ret = (time_t) -1;

  /*
   * Try to get the next sunrise/sunset time, relatively to the given
   * `date`. It might be the case that the sunrise/sunset time is earlier
   * than `date`; in this case, we get sunrise/sunset from the date + 24h.
   *
   * We retry 4 times, because it might be the case that sunrise is on the
   * previous UTC day, and taking into account MAX_SUNRISE_OFFSET_SEC, 4
   * is enough.
   */
  int i;
  time_t date_cur = date - MAX_SUNRISE_OFFSET_SEC;
  ret = 0;
  for (i = 0; i < 4 && ret <= date; i++, date_cur += ONE_DAY) {
    ret = s_get_sun_time2(date_cur, ce->expr.data.sun.type, lat, lon);
    ret += ce->expr.data.sun.offset;
    s_print_time(LL_DEBUG, "  Considering", ret);
  }

  assert(ret > date);

  return ret;
}

/*
 * Uses the specified expression to calculate the next fire date after the
 * specified date.
 */
static time_t s_cron_next(struct mgos_cron_entry *ce, time_t date) {
  time_t ret = (time_t) -1;

  switch (ce->expr.type) {
    case MGOS_CRON_EXPR_TYPE_NONE:
      LOG(LL_ERROR, ("NONE is an invalid type here"));
      abort();

    case MGOS_CRON_EXPR_TYPE_CCRONEXPR:
      ret = cron_next(ce->expr.data.ccronexpr, date);
      goto clean;

    case MGOS_CRON_EXPR_TYPE_RANDOM: {
      /*
       * First of all, we determine the `from` and `to` points, closest to the
       * given date. If `from` turns out to be later than `to`, it means that
       * the `date` is inside of the time window.
       */
      time_t from = cron_next(ce->expr.data.random.from, date);
      time_t to = cron_next(ce->expr.data.random.to, date);

      if (from == to) {
        LOG(LL_ERROR, ("From and to can't be the same"));
        goto clean;
      }

      if (from > to) {
        /*
         * Given `date` is inside of the time window; so first of all we'll
         * try to get the next point between the `date` and `to`.
         */
        time_t next_to = cron_next(ce->expr.data.random.to, from);
        time_t diff = next_to - from;
        LOG(LL_DEBUG, ("Trying to schedule inside of the current window"));

        ret = s_next_random(to - diff, to, date, ce->expr.data.random.number);
        if (ret != (time_t) -1) {
          /* Yes, we've got the next point in the current time window. */
          goto clean;
        } else {
          /*
           * Failed to get any points in the current time window, so fallback
           * to the next time window (in the future).
           */
          LOG(LL_DEBUG, ("Nothing scheduled inside of the current window"));
          to = next_to;
        }
      }
      LOG(LL_DEBUG, ("Scheduling outside of the window"));

      ret = s_next_random(from, to, date, ce->expr.data.random.number);
      assert(ret != (time_t) -1);
      goto clean;
    }

    case MGOS_CRON_EXPR_TYPE_SUN: {
      struct mgos_location_lat_lon loc;
      if (!mgos_location_get(&loc)) {
        LOG(LL_ERROR, ("Failed to get location"));
        goto clean;
      }

      /*
       * Consider days (in localtime) at which sunrise/sunset job should fire
       * (based on the cron expression: e.g. given the expr
       *     @sunrise * * * MON-FRI
       * the job won't fire on weekends).
       *
       * It might not happen at the first day if it's today, in which case we
       * only consider the period from now to the end of the day. It should
       * always happen on the next day (we don't yet account for polar day
       * and night), thus we loop 2 times.
       */
      time_t start = date - ONE_DAY - 1 /*compensate "start + 1" below*/;
      time_t end;
      int i;

      for (i = 0; i < 2; i++) {
        start = cron_next(ce->expr.data.sun.daybegin, start + 1);
        end = start + ONE_DAY;

        /*
         * If today is one of those days at which cron job should fire, then
         * the first "day" is from now to the end of the day
         */
        if (start < date) {
          start = date;
        }

        s_print_time(LL_INFO, "Looking for next sunrise/sunset after", start);
        s_print_time(LL_INFO, "And before", end);
        ret = s_get_sun_time(ce, start, loc.lat, loc.lon);
        assert(ret > start);

        if (ret < end) {
          /* Found the satisfying value */
          goto clean;
        } else {
          LOG(LL_DEBUG, ("Resort to the next day..."));
        }
      }

      LOG(LL_ERROR, ("Failed to get next sunrise/sunset time!"));
      goto clean;
    }
  }

  LOG(LL_ERROR, ("invalid expr type: %d", ce->expr.type));
  abort();

clean:
  s_print_time(LL_INFO, "Next invocation", ret);
  LOG(LL_DEBUG, ("(after %d seconds)", (int) (ret - date)));

  return ret;
}

time_t mgos_cron_get_next_invocation(mgos_cron_id_t id, time_t date) {
  if (id == MGOS_INVALID_CRON_ID) return 0;
  return s_cron_next((struct mgos_cron_entry *) id, date);
}

bool s_cron_schedule_next(struct mgos_cron_entry *ce) {
  /*
   * Schedule next time to fire (relatively to the saved ce->planned_time, so
   * we won't accumulate the delay), and also calculate how much time is left
   * until the next invocation. If it turns out that the next ivocation should
   * already have happened (unlikely for typical cron applications, but still),
   * then reschedule from now.
   */
  double left_until_next = 0.0;
  do {
    time_t prev_planned_time = ce->planned_time;
    ce->planned_time = s_cron_next(ce, prev_planned_time);
    if (ce->planned_time == (time_t) -1) {
      /* ccronexpr failed to parse the expression */
      return false;
    }
    left_until_next = ((double) ce->planned_time - mg_time()) * 1000.0;
    if (left_until_next < 0.0) {
      ce->planned_time = mg_time();
    }
  } while (left_until_next < 0.0);

  ce->timer_id = mgos_set_timer(left_until_next, false, s_cron_timer_cb, ce);
  if (ce->timer_id == MGOS_INVALID_TIMER_ID) {
    return false;
  }

  return true;
}

static void s_cron_timer_cb(void *arg) {
  struct mgos_cron_entry *ce = (struct mgos_cron_entry *) arg;
  if (!s_cron_schedule_next(ce)) {
    LOG(LL_ERROR, ("Failed to reschedule a cron job"));
  }
  ce->cb(ce->user_data, (mgos_cron_id_t) ce);
}

/*
 * Parses duration strings like "2h45m", "1.5h", "30s", and returns the
 * duration in seconds. If there's no trailing suffix, like "123", seconds
 * are assumed.
 *
 * In case of an error, 0 is returned, and *err is set to the error message;
 * client code should NOT free it.
 */
static int s_parse_duration(const char *str, const char **end,
                            const char **err) {
  int ret = 0;

  while (*str != '\0' && *str != ' ') {
    const char *end = NULL;
    double cur = strtod(str, (char **) &end);
    if (end == str) {
      *err = "invalid duration spec";
      goto clean;
    }

    str = end;

    switch (*str) {
      case 'h':
        cur *= 60 * 60;
        str++;
        break;
      case 'm':
        cur *= 60;
        str++;
        break;
      case 's':
        str++;
        break;
      case '\0':
        /* do nothing */
        break;
      default:
        *err = "invalid duration spec";
        goto clean;
    }

    ret += (int) cur;
  }

clean:
  if (*err != NULL) {
    ret = 0;
  }
  *end = str;
  return ret;
}

static int s_cron_parse_offset(const char *str, const char **end,
                               const char **err) {
  int ret = 0;
  bool neg = false;

  if (*str != '-' && *str != '+') {
    *err = "offset must begin with '-' or '+'";
    goto clean;
  }

  neg = (*str == '-');
  str++;

  ret = s_parse_duration(str, end, err);
  if (*err != NULL) {
    goto clean;
  }

  if (ret > MAX_SUNRISE_OFFSET_SEC) {
    *err = "offset must be not more than 12 hours";
    goto clean;
  }

  if (neg) {
    ret = -ret;
  }

clean:
  if (*err != NULL) {
    ret = 0;
  }
  LOG(LL_ERROR, ("returning offset=%d", ret));
  return ret;
}

static const char *s_cron_parse_expr_sun(const char *expr,
                                         struct mgos_cron_expr *tgt) {
  const char *err = NULL;
  char *cronexpr = NULL;

  size_t prefix_len = 0;
  const char *prefix = NULL;

  tgt->type = MGOS_CRON_EXPR_TYPE_SUN;

  if (prefix = SUNRISE_SPEC, prefix_len = strlen(prefix),
      strncmp(prefix, expr, prefix_len) == 0) {
    tgt->data.sun.type = MGOS_CRON_SUNRISE;
  } else if (prefix = SUNSET_SPEC, prefix_len = strlen(prefix),
             strncmp(prefix, expr, prefix_len) == 0) {
    tgt->data.sun.type = MGOS_CRON_SUNSET;
  } else {
    /* Should never be here */
    abort();
  }

  expr += prefix_len;
  if (*expr != ' ' && *expr != '\0') {
    /* Expr contains offset */
    tgt->data.sun.offset = s_cron_parse_offset(expr, &expr, &err);
    if (err != NULL) {
      goto clean;
    }
  }

  if (*expr == '\0') {
    /* Assume that this job should run every day */
    expr = " * * *";
  }

  /* Generate a regular cron expression for 00:00:00 at the specified days */
  mg_asprintf(&cronexpr, 0, "0 0 0%s", expr);
  tgt->data.sun.daybegin = cron_parse_expr(cronexpr, &err);
  if (tgt->data.sun.daybegin == NULL) {
    goto clean;
  }

clean:
  free(cronexpr);
  return err;
}

static const char *s_cron_parse_expr(const char *expr,
                                     struct mgos_cron_expr *tgt) {
  const char *err = NULL;
  char *from = NULL;
  char *to = NULL;
  memset(tgt, 0, sizeof(*tgt));

  if (expr[0] == '@') {
    /* The expression is extended, so we should handle it manually */
    const char *prefix = NULL;
    size_t prefix_len = 0;

    if (prefix = "@random:", prefix_len = strlen(prefix),
        strncmp(prefix, expr, prefix_len) == 0) {
      tgt->type = MGOS_CRON_EXPR_TYPE_RANDOM;
      if (json_scanf(expr + prefix_len, strlen(expr) - prefix_len,
                     "{from: %Q, to: %Q, number: %d}", &from, &to,
                     &tgt->data.random.number) != 3) {
        err = "from, to, and number should be present";
        goto clean;
      }

      if (strcmp(from, to) == 0) {
        // Both expressions (from and to) are the same, this is illegal.
        // NOTE that the opposite (expressions are not the same) is not a
        // guarantee that they won't evaluate to the same time on any possible
        // inputs; but checking it here is not trivial, so in those cases
        // the error will happen at the scheduling time.
        err = "from and to can't be the same";
        goto clean;
      }

      tgt->data.random.from = cron_parse_expr(from, &err);
      if (tgt->data.random.from == NULL) {
        goto clean;
      }

      tgt->data.random.to = cron_parse_expr(to, &err);
      if (tgt->data.random.to == NULL) {
        goto clean;
      }
    } else if ((prefix = SUNRISE_SPEC, prefix_len = strlen(prefix),
                strncmp(prefix, expr, prefix_len) == 0) ||
               (prefix = SUNSET_SPEC, prefix_len = strlen(prefix),
                strncmp(prefix, expr, prefix_len) == 0)) {
      err = s_cron_parse_expr_sun(expr, tgt);
      if (err != NULL) {
        goto clean;
      }
    } else {
      err = "Unknown @-expr";
      goto clean;
    }
  } else {
    /* The expression is a standard cron expr, so hand it over to ccronexpr */
    tgt->type = MGOS_CRON_EXPR_TYPE_CCRONEXPR;
    tgt->data.ccronexpr = cron_parse_expr(expr, &err);
    if (tgt->data.ccronexpr == NULL) {
      goto clean;
    }
  }

clean:
  free(from);
  free(to);
  if (err != NULL) {
    tgt->type = MGOS_CRON_EXPR_TYPE_NONE;
  }
  return err;
}

mgos_cron_id_t mgos_cron_add(const char *expr, mgos_cron_callback_t cb,
                             void *user_data) {
  bool ok = false;
  struct mgos_cron_entry *ce =
      (struct mgos_cron_entry *) calloc(1, sizeof(*ce));
  const char *err = NULL;

  if (ce == NULL) goto clean;

  err = s_cron_parse_expr(expr, &ce->expr);
  if (err != NULL) {
    LOG(LL_ERROR, ("failed to parse cron expr \"%s\": %s", expr, err));
    goto clean;
  }

  /*
   * Set planned_time to now, so that s_cron_schedule_next will calculate the
   * next invocation time relative to now. Further invocation times will be
   * calculated relatively to the previous.
   */
  ce->planned_time = mg_time();
  ce->cb = cb;
  ce->user_data = user_data;

  if (!s_cron_schedule_next(ce)) {
    goto clean;
  }

  ok = s_cron_add(ce);

clean:
  if (!ok) {
    s_cron_entry_free(ce);
    free(ce);
    return MGOS_INVALID_CRON_ID;
  }
  return (mgos_cron_id_t) ce;
}

void *mgos_cron_get_user_data(mgos_cron_id_t id) {
  struct mgos_cron_entry *ce = (struct mgos_cron_entry *) id;
  if (ce == NULL) return NULL;
  return ce->user_data;
}

bool mgos_cron_is_expr_valid(const char *expr, const char **perr) {
  struct mgos_cron_entry ce;
  memset(&ce, 0, sizeof(ce));

  const char *err = s_cron_parse_expr(expr, &ce.expr);
  if (err != NULL) {
    if (perr != NULL) {
      *perr = err;
    }
    return false;
  }

  s_cron_entry_free(&ce);
  return true;
}

void mgos_cron_remove(mgos_cron_id_t id) {
  if (id == MGOS_INVALID_CRON_ID) return;
  s_cron_remove((struct mgos_cron_entry *) id);
}

static void time_change_cb2(void *arg) {
  struct mgos_cron_entry *ce;
  SLIST_FOREACH(ce, &s_cron_entries, entries) {
    LOG(LL_DEBUG, ("Rescheduling cron job %p after time change", ce));
    mgos_clear_timer(ce->timer_id);
    ce->timer_id = MGOS_INVALID_TIMER_ID;
    ce->planned_time = mg_time();
    if (!s_cron_schedule_next(ce)) {
      LOG(LL_DEBUG, ("Failed to reschedule"));
    }
  }

  (void) arg;
}

static void time_change_cb(int ev, void *ev_data, void *arg) {
  /*
   * We can't manage timer jobs right here (in the time change callback),
   * because the timer also adjusts its jobs in its own time change callback,
   * and thus it will do `+= delta` on every job we setup here. To avoid that,
   * we reschedule cron jobs separately from the main mOS event loop.
   */
  mgos_invoke_cb(time_change_cb2, NULL, false);

  (void) ev;
  (void) ev_data;
  (void) arg;
}

bool mgos_cron_init(void) {
  mgos_event_add_handler(MGOS_EVENT_TIME_CHANGED, time_change_cb, NULL);

  return true;
}
