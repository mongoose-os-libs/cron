
typedef void (*mgos_sntp_time_change_cb)(void *arg, double delta);
static inline void mgos_sntp_add_time_change_cb(mgos_sntp_time_change_cb cb,
                                                void *arg) {
  (void) cb;
  (void) arg;
}
