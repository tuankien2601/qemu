// MPTable generation (on emulators)
//
// Copyright (C) 2008,2009  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2006 Fabrice Bellard
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "util.h" // dprintf
#include "config.h" // CONFIG_*
#include "mptable.h" // MPTABLE_SIGNATURE
#include "paravirt.h" // qemu_cfg_irq0_override

void
mptable_init(void)
{
    if (! CONFIG_MPTABLE)
        return;

    dprintf(3, "init MPTable\n");

    // Allocate memory
    int length = (sizeof(struct mptable_config_s)
                  + sizeof(struct mpt_cpu) * MaxCountCPUs
                  + sizeof(struct mpt_bus)
                  + sizeof(struct mpt_ioapic)
                  + sizeof(struct mpt_intsrc) * 18);
    struct mptable_config_s *config = malloc_fseg(length);
    struct mptable_floating_s *floating = malloc_fseg(sizeof(*floating));
    if (!config || !floating) {
        dprintf(1, "No room for MPTABLE!\n");
        free(config);
        free(floating);
        return;
    }

    /* floating pointer structure */
    memset(floating, 0, sizeof(*floating));
    floating->signature = MPTABLE_SIGNATURE;
    floating->physaddr = (u32)config;
    floating->length = 1;
    floating->spec_rev = 4;
    floating->checksum -= checksum(floating, sizeof(*floating));

    // Config structure.
    memset(config, 0, sizeof(*config));
    config->signature = MPCONFIG_SIGNATURE;
    config->spec = 4;
    memcpy(config->oemid, CONFIG_CPUNAME8, sizeof(config->oemid));
    memcpy(config->productid, "0.1         ", sizeof(config->productid));
    config->lapic = BUILD_APIC_ADDR;

    // Detect cpu info
    u32 cpuid_signature, ebx, ecx, cpuid_features;
    cpuid(1, &cpuid_signature, &ebx, &ecx, &cpuid_features);
    int pkgcpus = 1;
    if (cpuid_features & (1 << 28)) {
        /* Only populate the MPS tables with the first logical CPU in
           each package */
        pkgcpus = (ebx >> 16) & 0xff;
        pkgcpus = 1 << (__fls(pkgcpus - 1) + 1); /* round up to power of 2 */
    }

    // CPU definitions.
    struct mpt_cpu *cpus = (void*)&config[1], *cpu = cpus;
    int i;
    for (i = 0; i < MaxCountCPUs; i+=pkgcpus) {
        memset(cpu, 0, sizeof(*cpu));
        cpu->type = MPT_TYPE_CPU;
        cpu->apicid = i;
        cpu->apicver = 0x11;
        /* cpu flags: enabled, bootstrap cpu */
        cpu->cpuflag = (i < CountCPUs) | ((i == 0) << 1);
        if (cpuid_signature) {
            cpu->cpusignature = cpuid_signature;
            cpu->featureflag = cpuid_features;
        } else {
            cpu->cpusignature = 0x600;
            cpu->featureflag = 0x201;
        }
        cpu++;
    }
    int entrycount = cpu - cpus;

    /* isa bus */
    struct mpt_bus *bus = (void*)cpu;
    memset(bus, 0, sizeof(*bus));
    bus->type = MPT_TYPE_BUS;
    memcpy(bus->bustype, "ISA   ", sizeof(bus->bustype));
    entrycount++;

    /* ioapic */
    u8 ioapic_id = CountCPUs;
    struct mpt_ioapic *ioapic = (void*)&bus[1];
    memset(ioapic, 0, sizeof(*ioapic));
    ioapic->type = MPT_TYPE_IOAPIC;
    ioapic->apicid = ioapic_id;
    ioapic->apicver = 0x11;
    ioapic->flags = 1; // enable
    ioapic->apicaddr = BUILD_IOAPIC_ADDR;
    entrycount++;

    /* irqs */
    struct mpt_intsrc *intsrcs = (void*)&ioapic[1], *intsrc = intsrcs;
    for (i = 0; i < 16; i++) {
        memset(intsrc, 0, sizeof(*intsrc));
        intsrc->type = MPT_TYPE_INTSRC;
        intsrc->srcbusirq = i;
        intsrc->dstapic = ioapic_id;
        intsrc->dstirq = i;
        if (qemu_cfg_irq0_override()) {
            /* Destination 2 is covered by irq0->inti2 override (i ==
               0). Source IRQ 2 is unused */
            if (i == 0)
                intsrc->dstirq = 2;
            else if (i == 2)
                intsrc--;
        }
        intsrc++;
    }
    entrycount += intsrc - intsrcs;

    /* Local interrupt assignment */
    intsrc->type = MPT_TYPE_LOCAL_INT;
    intsrc->irqtype = 3; /* ExtINT */
    intsrc->irqflag = 0; /* PO, EL default */
    intsrc->srcbus = 0;
    intsrc->srcbusirq = 0;
    intsrc->dstapic = 0; /* BSP == APIC #0 */
    intsrc->dstirq = 0; /* LINTIN0 */
    intsrc++;
    entrycount++;

    intsrc->type = MPT_TYPE_LOCAL_INT;
    intsrc->irqtype = 1; /* NMI */
    intsrc->irqflag = 0; /* PO, EL default */
    intsrc->srcbus = 0;
    intsrc->srcbusirq = 0;
    intsrc->dstapic = 0; /* BSP == APIC #0 */
    intsrc->dstirq = 1; /* LINTIN1 */
    intsrc++;
    entrycount++;

    // Finalize config structure.
    config->entrycount = entrycount;
    config->length = (void*)intsrc - (void*)config;
    config->checksum -= checksum(config, config->length);

    dprintf(1, "MP table addr=%p MPC table addr=%p size=%d\n",
            floating, config, config->length);
}
