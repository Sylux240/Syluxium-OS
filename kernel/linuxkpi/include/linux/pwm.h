/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: PWM devices - none (i915 programs its own backlight PWM). */
struct pwm_device;
struct pwm_state { u64 period, duty_cycle; int polarity; bool enabled; bool usage_power; };
#define pwm_get(dev, id) ((struct pwm_device *)ERR_PTR(-ENODEV))
#define devm_pwm_get(dev, id) ((struct pwm_device *)ERR_PTR(-ENODEV))
#define pwm_put(p) do { } while (0)
#define pwm_apply_might_sleep(p, s) (-ENODEV)
#define pwm_apply_state(p, s) (-ENODEV)
#define pwm_get_state(p, s) memset(s, 0, sizeof(*(s)))
#define pwm_init_state(p, s) memset(s, 0, sizeof(*(s)))
#define pwm_get_relative_duty_cycle(s, scale) 0
#define pwm_set_relative_duty_cycle(s, d, scale) 0
#define pwm_add_table(t, n) do { } while (0)
#define pwm_remove_table(t, n) do { } while (0)
struct pwm_lookup { const char *provider; unsigned int index; const char *dev_id, *con_id; u64 period; int polarity; };
#define PWM_LOOKUP(_p, _i, _d, _c, _per, _pol) { .provider = _p, .index = _i, .dev_id = _d, .con_id = _c, .period = _per, .polarity = _pol }
#define PWM_POLARITY_NORMAL 0
#define PWM_POLARITY_INVERSED 1
