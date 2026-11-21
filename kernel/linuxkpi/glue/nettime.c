/* LinuxKPI: time counters over a device's cycle counter (Linux
 * kernel/time/timecounter.c) and the legacy ethtool link-mode conversions
 * (net/ethtool/ioctl.c), as network drivers use them. */
#include <linux/lkpi_all.h>
#include <linux/timecounter.h>
#include <linux/ethtool.h>
#include <linux/linkmode.h>

void timecounter_init(struct timecounter *tc, const struct cyclecounter *cc, u64 start_tstamp)
{
	tc->cc = cc;
	tc->cycle_last = cc->read(cc);
	tc->nsec = start_tstamp;
	tc->mask = (1ULL << cc->shift) - 1;
	tc->frac = 0;
}

/* Nanoseconds since the last call; the counter must not wrap twice in between. */
static u64 timecounter_read_delta(struct timecounter *tc)
{
	u64 cycle_now = tc->cc->read(tc->cc);
	u64 cycle_delta = (cycle_now - tc->cycle_last) & tc->cc->mask;
	u64 ns_offset = cyclecounter_cyc2ns(tc->cc, cycle_delta, tc->mask, &tc->frac);

	tc->cycle_last = cycle_now;
	return ns_offset;
}

u64 timecounter_read(struct timecounter *tc)
{
	tc->nsec += timecounter_read_delta(tc);
	return tc->nsec;
}

static u64 cc_cyc2ns_backwards(const struct cyclecounter *cc, u64 cycles, u64 mask, u64 frac)
{
	u64 ns = (u64)cycles;

	ns = ((ns * cc->mult) - frac) >> cc->shift;
	return ns;
}

u64 timecounter_cyc2time(const struct timecounter *tc, u64 cycle_tstamp)
{
	u64 delta = (cycle_tstamp - tc->cycle_last) & tc->cc->mask;
	u64 nsec = tc->nsec, frac = tc->frac;

	/* A time stamp from before the last read: count backwards. */
	if (delta > tc->cc->mask / 2) {
		delta = (tc->cycle_last - cycle_tstamp) & tc->cc->mask;
		nsec -= cc_cyc2ns_backwards(tc->cc, delta, tc->mask, frac);
	} else {
		nsec += cyclecounter_cyc2ns(tc->cc, delta, tc->mask, &frac);
	}
	return nsec;
}

/* ethtool string tables: fixed ETH_GSTRING_LEN slots. */
void ethtool_sprintf(u8 **data, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf((char *)*data, ETH_GSTRING_LEN, fmt, args);
	va_end(args);
	*data += ETH_GSTRING_LEN;
}

void ethtool_puts(u8 **data, const char *str)
{
	strscpy((char *)*data, str, ETH_GSTRING_LEN);
	*data += ETH_GSTRING_LEN;
}

void ethtool_convert_legacy_u32_to_link_mode(unsigned long *dst, u32 legacy_u32)
{
	linkmode_zero(dst);
	dst[0] = legacy_u32;
}

bool ethtool_convert_link_mode_to_legacy_u32(u32 *legacy_u32, const unsigned long *src)
{
	bool retval = true;

	/* Modes beyond the first 32 can't be expressed: report that. */
	if (__ETHTOOL_LINK_MODE_MASK_NBITS > 32) {
		__ETHTOOL_DECLARE_LINK_MODE_MASK(ext);
		linkmode_zero(ext);
		bitmap_fill(ext, 32);
		bitmap_complement(ext, ext, __ETHTOOL_LINK_MODE_MASK_NBITS);
		if (bitmap_intersects(ext, src, __ETHTOOL_LINK_MODE_MASK_NBITS))
			retval = false;
	}
	*legacy_u32 = src[0];
	return retval;
}
