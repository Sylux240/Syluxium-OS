// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI boot self-test of phylib (QEMU emulates no Ethernet chip with an
 * MDIO PHY): an MDIO bus whose register file looks like a Realtek RTL8211F
 * gigabit PHY at address 1 is registered. The bus scan must create the PHY
 * device, the driver model must bind the Realtek PHY driver to it by its ID,
 * and the driver's probe/config must run against the emulated registers -
 * the path r8169 takes on real hardware. Then everything is taken down.
 */
#include <net/lkpi_phy.h>
#include <linux/phy.h>
#include <linux/mii.h>

#define TEST_ADDR 1

struct fake_phy {
	u16 regs[8][32]; /* page (reg 0x1f selects) x register */
	int page;
};

static int fake_read(struct mii_bus *bus, int addr, int reg)
{
	struct fake_phy *p = bus->priv;

	if (addr != TEST_ADDR)
		return 0xffff; /* nobody there */
	if (reg == 0x1f)
		return p->page;
	return p->regs[p->page & 7][reg & 31];
}

static int fake_write(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct fake_phy *p = bus->priv;

	if (addr != TEST_ADDR)
		return 0;
	if (reg == 0x1f) {
		p->page = val;
		return 0;
	}
	if (reg == MII_BMCR)
		val &= ~BMCR_RESET; /* resets complete at once */
	p->regs[p->page & 7][reg & 31] = val;
	return 0;
}

static int __init lkpi_phy_selftest(void)
{
	struct fake_phy *p;
	struct mii_bus *bus;
	struct phy_device *phy;
	int err, ok = 0;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	bus = mdiobus_alloc();
	if (!p || !bus) {
		kfree(p);
		mdiobus_free(bus);
		return 0;
	}
	p->regs[0][MII_BMCR] = BMCR_ANENABLE | BMCR_FULLDPLX | BMCR_SPEED1000;
	p->regs[0][MII_BMSR] = BMSR_100FULL | BMSR_100HALF | BMSR_10FULL | BMSR_10HALF | BMSR_ESTATEN |
			       BMSR_ANEGCOMPLETE | BMSR_ANEGCAPABLE | BMSR_LSTATUS | BMSR_ERCAP;
	p->regs[0][MII_PHYSID1] = 0x001c;
	p->regs[0][MII_PHYSID2] = 0xc916; /* RTL8211F */
	p->regs[0][MII_ESTATUS] = ESTATUS_1000_TFULL | ESTATUS_1000_THALF;

	bus->name = "lkpi-phy-selftest";
	snprintf(bus->id, MII_BUS_ID_SIZE, "lkpi-selftest");
	bus->priv = p;
	bus->read = fake_read;
	bus->write = fake_write;
	bus->phy_mask = ~BIT(TEST_ADDR);
	err = mdiobus_register(bus);
	if (err) {
		pr_err("phylib self-test: registering the MDIO bus failed (%d)\n", err);
		goto out;
	}
	phy = mdiobus_get_phy(bus, TEST_ADDR);
	if (!phy)
		pr_err("phylib self-test: the bus scan found no PHY\n");
	else if (!phy->drv)
		pr_err("phylib self-test: PHY %08x found, but no driver bound to it\n", phy->phy_id);
	else {
		pr_info("phylib self-test: PHY %08x on an emulated MDIO bus, driver \"%s\" bound and probed\n",
			phy->phy_id, phy->drv->name);
		ok = 1;
	}
	mdiobus_unregister(bus);
out:
	mdiobus_free(bus);
	kfree(p);
	if (!ok)
		pr_err("phylib self-test failed: r8169 will not find its PHY\n");
	return 0;
}
late_initcall(lkpi_phy_selftest);
