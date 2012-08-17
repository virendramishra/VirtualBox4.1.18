/* $Id: DevE1000.cpp $ */
/** @file
 * DevE1000 - Intel 82540EM Ethernet Controller Emulation.
 *
 * Implemented in accordance with the specification:
 *
 *      PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's Manual
 *      82540EP/EM, 82541xx, 82544GC/EI, 82545GM/EM, 82546GB/EB, and 82547xx
 *
 *      317453-002 Revision 3.5
 *
 * @todo IPv6 checksum offloading support
 * @todo VLAN checksum offloading support
 * @todo Flexible Filter / Wakeup (optional?)
 */

/*
 * Copyright (C) 2007-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#define LOG_GROUP LOG_GROUP_DEV_E1000

//#define E1kLogRel(a) LogRel(a)
#define E1kLogRel(a)

/* Options */
#define E1K_INIT_RA0
#define E1K_LSC_ON_SLU
#define E1K_ITR_ENABLED
//#define E1K_GLOBAL_MUTEX
//#define E1K_USE_TX_TIMERS
//#define E1K_NO_TAD
//#define E1K_REL_DEBUG
//#define E1K_INT_STATS
//#define E1K_REL_STATS
//#define E1K_USE_SUPLIB_SEMEVENT
//#define E1K_WITH_MSI

#include <iprt/crc.h>
#include <iprt/ctype.h>
#include <iprt/net.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/uuid.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/pdmnetifs.h>
#include <VBox/vmm/pdmnetinline.h>
#include <VBox/param.h>
#include "VBoxDD.h"

#include "DevEEPROM.h"
#include "DevE1000Phy.h"

/* Little helpers ************************************************************/
#undef htons
#undef ntohs
#undef htonl
#undef ntohl
#define htons(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#define ntohs(x) htons(x)
#define htonl(x) ASMByteSwapU32(x)
#define ntohl(x) htonl(x)

#ifndef DEBUG
# ifdef E1K_REL_STATS
#  undef STAM_COUNTER_INC
#  undef STAM_PROFILE_ADV_START
#  undef STAM_PROFILE_ADV_STOP
#  define STAM_COUNTER_INC       STAM_REL_COUNTER_INC
#  define STAM_PROFILE_ADV_START STAM_REL_PROFILE_ADV_START
#  define STAM_PROFILE_ADV_STOP  STAM_REL_PROFILE_ADV_STOP
# endif
# ifdef E1K_REL_DEBUG
#  define DEBUG
#  define E1kLog(a)               LogRel(a)
#  define E1kLog2(a)              LogRel(a)
#  define E1kLog3(a)              LogRel(a)
//#  define E1kLog3(a)              do {} while (0)
# else
#  define E1kLog(a)               do {} while (0)
#  define E1kLog2(a)              do {} while (0)
#  define E1kLog3(a)              do {} while (0)
# endif
#else
#  define E1kLog(a)               Log(a)
#  define E1kLog2(a)              Log2(a)
#  define E1kLog3(a)              Log3(a)
//#  define E1kLog(a)               do {} while (0)
//#  define E1kLog2(a)              do {} while (0)
//#  define E1kLog3(a)              do {} while (0)
#endif

//#undef DEBUG

#define INSTANCE(pState) pState->szInstance
#define STATE_TO_DEVINS(pState)           (((E1KSTATE *)pState)->CTX_SUFF(pDevIns))
#define E1K_RELOCATE(p, o) *(RTHCUINTPTR *)&p += o

#define E1K_INC_CNT32(cnt) \
do { \
    if (cnt < UINT32_MAX) \
        cnt++; \
} while (0)

#define E1K_ADD_CNT64(cntLo, cntHi, val) \
do { \
    uint64_t u64Cnt = RT_MAKE_U64(cntLo, cntHi); \
    uint64_t tmp  = u64Cnt; \
    u64Cnt += val; \
    if (tmp > u64Cnt ) \
        u64Cnt = UINT64_MAX; \
    cntLo = (uint32_t)u64Cnt; \
    cntHi = (uint32_t)(u64Cnt >> 32); \
} while (0)

#ifdef E1K_INT_STATS
# define E1K_INC_ISTAT_CNT(cnt) ++cnt
#else /* E1K_INT_STATS */
# define E1K_INC_ISTAT_CNT(cnt)
#endif /* E1K_INT_STATS */


/*****************************************************************************/

typedef uint32_t E1KCHIP;
#define E1K_CHIP_82540EM 0
#define E1K_CHIP_82543GC 1
#define E1K_CHIP_82545EM 2

struct E1kChips
{
    uint16_t uPCIVendorId;
    uint16_t uPCIDeviceId;
    uint16_t uPCISubsystemVendorId;
    uint16_t uPCISubsystemId;
    const char *pcszName;
} g_Chips[] =
{
    /* Vendor Device SSVendor SubSys  Name */
    { 0x8086,
      /* Temporary code, as MSI-aware driver dislike 0x100E. How to do that right? */
#ifdef E1K_WITH_MSI
      0x105E,
#else
      0x100E,
#endif
                      0x8086, 0x001E, "82540EM" }, /* Intel 82540EM-A in Intel PRO/1000 MT Desktop */
    { 0x8086, 0x1004, 0x8086, 0x1004, "82543GC" }, /* Intel 82543GC   in Intel PRO/1000 T  Server */
    { 0x8086, 0x100F, 0x15AD, 0x0750, "82545EM" }  /* Intel 82545EM-A in VMWare Network Adapter */
};


/* The size of register area mapped to I/O space */
#define E1K_IOPORT_SIZE                 0x8
/* The size of memory-mapped register area */
#define E1K_MM_SIZE                     0x20000

#define E1K_MAX_TX_PKT_SIZE    16288
#define E1K_MAX_RX_PKT_SIZE    16384

/*****************************************************************************/

/** Gets the specfieid bits from the register. */
#define GET_BITS(reg, bits) ((reg & reg##_##bits##_MASK) >> reg##_##bits##_SHIFT)
#define GET_BITS_V(val, reg, bits) ((val & reg##_##bits##_MASK) >> reg##_##bits##_SHIFT)
#define BITS(reg, bits, bitval) (bitval << reg##_##bits##_SHIFT)
#define SET_BITS(reg, bits, bitval) do { reg = (reg & ~reg##_##bits##_MASK) | (bitval << reg##_##bits##_SHIFT); } while (0)
#define SET_BITS_V(val, reg, bits, bitval) do { val = (val & ~reg##_##bits##_MASK) | (bitval << reg##_##bits##_SHIFT); } while (0)

#define CTRL_SLU        0x00000040
#define CTRL_MDIO       0x00100000
#define CTRL_MDC        0x00200000
#define CTRL_MDIO_DIR   0x01000000
#define CTRL_MDC_DIR    0x02000000
#define CTRL_RESET      0x04000000
#define CTRL_VME        0x40000000

#define STATUS_LU       0x00000002

#define EECD_EE_WIRES 0x0F
#define EECD_EE_REQ   0x40
#define EECD_EE_GNT   0x80

#define EERD_START       0x00000001
#define EERD_DONE        0x00000010
#define EERD_DATA_MASK   0xFFFF0000
#define EERD_DATA_SHIFT  16
#define EERD_ADDR_MASK   0x0000FF00
#define EERD_ADDR_SHIFT  8

#define MDIC_DATA_MASK  0x0000FFFF
#define MDIC_DATA_SHIFT 0
#define MDIC_REG_MASK   0x001F0000
#define MDIC_REG_SHIFT  16
#define MDIC_PHY_MASK   0x03E00000
#define MDIC_PHY_SHIFT  21
#define MDIC_OP_WRITE   0x04000000
#define MDIC_OP_READ    0x08000000
#define MDIC_READY      0x10000000
#define MDIC_INT_EN     0x20000000
#define MDIC_ERROR      0x40000000

#define TCTL_EN         0x00000002
#define TCTL_PSP        0x00000008

#define RCTL_EN          0x00000002
#define RCTL_UPE         0x00000008
#define RCTL_MPE         0x00000010
#define RCTL_LPE         0x00000020
#define RCTL_LBM_MASK    0x000000C0
#define RCTL_LBM_SHIFT   6
#define RCTL_RDMTS_MASK  0x00000300
#define RCTL_RDMTS_SHIFT 8
#define RCTL_LBM_TCVR    3              /**< PHY or external SerDes loopback. */
#define RCTL_MO_MASK     0x00003000
#define RCTL_MO_SHIFT    12
#define RCTL_BAM         0x00008000
#define RCTL_BSIZE_MASK  0x00030000
#define RCTL_BSIZE_SHIFT 16
#define RCTL_VFE         0x00040000
#define RCTL_BSEX        0x02000000
#define RCTL_SECRC       0x04000000

#define ICR_TXDW         0x00000001
#define ICR_TXQE         0x00000002
#define ICR_LSC          0x00000004
#define ICR_RXDMT0       0x00000010
#define ICR_RXT0         0x00000080
#define ICR_TXD_LOW      0x00008000
#define RDTR_FPD         0x80000000

#define PBA_st  ((PBAST*)(pState->auRegs + PBA_IDX))
typedef struct
{
    unsigned rxa   : 7;
    unsigned rxa_r : 9;
    unsigned txa   : 16;
} PBAST;
AssertCompileSize(PBAST, 4);

#define TXDCTL_WTHRESH_MASK   0x003F0000
#define TXDCTL_WTHRESH_SHIFT  16
#define TXDCTL_LWTHRESH_MASK  0xFE000000
#define TXDCTL_LWTHRESH_SHIFT 25

#define RXCSUM_PCSS_MASK  0x000000FF
#define RXCSUM_PCSS_SHIFT 0

/* Register access macros ****************************************************/
#define CTRL     pState->auRegs[CTRL_IDX]
#define STATUS   pState->auRegs[STATUS_IDX]
#define EECD     pState->auRegs[EECD_IDX]
#define EERD     pState->auRegs[EERD_IDX]
#define CTRL_EXT pState->auRegs[CTRL_EXT_IDX]
#define FLA      pState->auRegs[FLA_IDX]
#define MDIC     pState->auRegs[MDIC_IDX]
#define FCAL     pState->auRegs[FCAL_IDX]
#define FCAH     pState->auRegs[FCAH_IDX]
#define FCT      pState->auRegs[FCT_IDX]
#define VET      pState->auRegs[VET_IDX]
#define ICR      pState->auRegs[ICR_IDX]
#define ITR      pState->auRegs[ITR_IDX]
#define ICS      pState->auRegs[ICS_IDX]
#define IMS      pState->auRegs[IMS_IDX]
#define IMC      pState->auRegs[IMC_IDX]
#define RCTL     pState->auRegs[RCTL_IDX]
#define FCTTV    pState->auRegs[FCTTV_IDX]
#define TXCW     pState->auRegs[TXCW_IDX]
#define RXCW     pState->auRegs[RXCW_IDX]
#define TCTL     pState->auRegs[TCTL_IDX]
#define TIPG     pState->auRegs[TIPG_IDX]
#define AIFS     pState->auRegs[AIFS_IDX]
#define LEDCTL   pState->auRegs[LEDCTL_IDX]
#define PBA      pState->auRegs[PBA_IDX]
#define FCRTL    pState->auRegs[FCRTL_IDX]
#define FCRTH    pState->auRegs[FCRTH_IDX]
#define RDFH     pState->auRegs[RDFH_IDX]
#define RDFT     pState->auRegs[RDFT_IDX]
#define RDFHS    pState->auRegs[RDFHS_IDX]
#define RDFTS    pState->auRegs[RDFTS_IDX]
#define RDFPC    pState->auRegs[RDFPC_IDX]
#define RDBAL    pState->auRegs[RDBAL_IDX]
#define RDBAH    pState->auRegs[RDBAH_IDX]
#define RDLEN    pState->auRegs[RDLEN_IDX]
#define RDH      pState->auRegs[RDH_IDX]
#define RDT      pState->auRegs[RDT_IDX]
#define RDTR     pState->auRegs[RDTR_IDX]
#define RXDCTL   pState->auRegs[RXDCTL_IDX]
#define RADV     pState->auRegs[RADV_IDX]
#define RSRPD    pState->auRegs[RSRPD_IDX]
#define TXDMAC   pState->auRegs[TXDMAC_IDX]
#define TDFH     pState->auRegs[TDFH_IDX]
#define TDFT     pState->auRegs[TDFT_IDX]
#define TDFHS    pState->auRegs[TDFHS_IDX]
#define TDFTS    pState->auRegs[TDFTS_IDX]
#define TDFPC    pState->auRegs[TDFPC_IDX]
#define TDBAL    pState->auRegs[TDBAL_IDX]
#define TDBAH    pState->auRegs[TDBAH_IDX]
#define TDLEN    pState->auRegs[TDLEN_IDX]
#define TDH      pState->auRegs[TDH_IDX]
#define TDT      pState->auRegs[TDT_IDX]
#define TIDV     pState->auRegs[TIDV_IDX]
#define TXDCTL   pState->auRegs[TXDCTL_IDX]
#define TADV     pState->auRegs[TADV_IDX]
#define TSPMT    pState->auRegs[TSPMT_IDX]
#define CRCERRS  pState->auRegs[CRCERRS_IDX]
#define ALGNERRC pState->auRegs[ALGNERRC_IDX]
#define SYMERRS  pState->auRegs[SYMERRS_IDX]
#define RXERRC   pState->auRegs[RXERRC_IDX]
#define MPC      pState->auRegs[MPC_IDX]
#define SCC      pState->auRegs[SCC_IDX]
#define ECOL     pState->auRegs[ECOL_IDX]
#define MCC      pState->auRegs[MCC_IDX]
#define LATECOL  pState->auRegs[LATECOL_IDX]
#define COLC     pState->auRegs[COLC_IDX]
#define DC       pState->auRegs[DC_IDX]
#define TNCRS    pState->auRegs[TNCRS_IDX]
#define SEC      pState->auRegs[SEC_IDX]
#define CEXTERR  pState->auRegs[CEXTERR_IDX]
#define RLEC     pState->auRegs[RLEC_IDX]
#define XONRXC   pState->auRegs[XONRXC_IDX]
#define XONTXC   pState->auRegs[XONTXC_IDX]
#define XOFFRXC  pState->auRegs[XOFFRXC_IDX]
#define XOFFTXC  pState->auRegs[XOFFTXC_IDX]
#define FCRUC    pState->auRegs[FCRUC_IDX]
#define PRC64    pState->auRegs[PRC64_IDX]
#define PRC127   pState->auRegs[PRC127_IDX]
#define PRC255   pState->auRegs[PRC255_IDX]
#define PRC511   pState->auRegs[PRC511_IDX]
#define PRC1023  pState->auRegs[PRC1023_IDX]
#define PRC1522  pState->auRegs[PRC1522_IDX]
#define GPRC     pState->auRegs[GPRC_IDX]
#define BPRC     pState->auRegs[BPRC_IDX]
#define MPRC     pState->auRegs[MPRC_IDX]
#define GPTC     pState->auRegs[GPTC_IDX]
#define GORCL    pState->auRegs[GORCL_IDX]
#define GORCH    pState->auRegs[GORCH_IDX]
#define GOTCL    pState->auRegs[GOTCL_IDX]
#define GOTCH    pState->auRegs[GOTCH_IDX]
#define RNBC     pState->auRegs[RNBC_IDX]
#define RUC      pState->auRegs[RUC_IDX]
#define RFC      pState->auRegs[RFC_IDX]
#define ROC      pState->auRegs[ROC_IDX]
#define RJC      pState->auRegs[RJC_IDX]
#define MGTPRC   pState->auRegs[MGTPRC_IDX]
#define MGTPDC   pState->auRegs[MGTPDC_IDX]
#define MGTPTC   pState->auRegs[MGTPTC_IDX]
#define TORL     pState->auRegs[TORL_IDX]
#define TORH     pState->auRegs[TORH_IDX]
#define TOTL     pState->auRegs[TOTL_IDX]
#define TOTH     pState->auRegs[TOTH_IDX]
#define TPR      pState->auRegs[TPR_IDX]
#define TPT      pState->auRegs[TPT_IDX]
#define PTC64    pState->auRegs[PTC64_IDX]
#define PTC127   pState->auRegs[PTC127_IDX]
#define PTC255   pState->auRegs[PTC255_IDX]
#define PTC511   pState->auRegs[PTC511_IDX]
#define PTC1023  pState->auRegs[PTC1023_IDX]
#define PTC1522  pState->auRegs[PTC1522_IDX]
#define MPTC     pState->auRegs[MPTC_IDX]
#define BPTC     pState->auRegs[BPTC_IDX]
#define TSCTC    pState->auRegs[TSCTC_IDX]
#define TSCTFC   pState->auRegs[TSCTFC_IDX]
#define RXCSUM   pState->auRegs[RXCSUM_IDX]
#define WUC      pState->auRegs[WUC_IDX]
#define WUFC     pState->auRegs[WUFC_IDX]
#define WUS      pState->auRegs[WUS_IDX]
#define MANC     pState->auRegs[MANC_IDX]
#define IPAV     pState->auRegs[IPAV_IDX]
#define WUPL     pState->auRegs[WUPL_IDX]

/**
 * Indices of memory-mapped registers in register table
 */
typedef enum
{
    CTRL_IDX,
    STATUS_IDX,
    EECD_IDX,
    EERD_IDX,
    CTRL_EXT_IDX,
    FLA_IDX,
    MDIC_IDX,
    FCAL_IDX,
    FCAH_IDX,
    FCT_IDX,
    VET_IDX,
    ICR_IDX,
    ITR_IDX,
    ICS_IDX,
    IMS_IDX,
    IMC_IDX,
    RCTL_IDX,
    FCTTV_IDX,
    TXCW_IDX,
    RXCW_IDX,
    TCTL_IDX,
    TIPG_IDX,
    AIFS_IDX,
    LEDCTL_IDX,
    PBA_IDX,
    FCRTL_IDX,
    FCRTH_IDX,
    RDFH_IDX,
    RDFT_IDX,
    RDFHS_IDX,
    RDFTS_IDX,
    RDFPC_IDX,
    RDBAL_IDX,
    RDBAH_IDX,
    RDLEN_IDX,
    RDH_IDX,
    RDT_IDX,
    RDTR_IDX,
    RXDCTL_IDX,
    RADV_IDX,
    RSRPD_IDX,
    TXDMAC_IDX,
    TDFH_IDX,
    TDFT_IDX,
    TDFHS_IDX,
    TDFTS_IDX,
    TDFPC_IDX,
    TDBAL_IDX,
    TDBAH_IDX,
    TDLEN_IDX,
    TDH_IDX,
    TDT_IDX,
    TIDV_IDX,
    TXDCTL_IDX,
    TADV_IDX,
    TSPMT_IDX,
    CRCERRS_IDX,
    ALGNERRC_IDX,
    SYMERRS_IDX,
    RXERRC_IDX,
    MPC_IDX,
    SCC_IDX,
    ECOL_IDX,
    MCC_IDX,
    LATECOL_IDX,
    COLC_IDX,
    DC_IDX,
    TNCRS_IDX,
    SEC_IDX,
    CEXTERR_IDX,
    RLEC_IDX,
    XONRXC_IDX,
    XONTXC_IDX,
    XOFFRXC_IDX,
    XOFFTXC_IDX,
    FCRUC_IDX,
    PRC64_IDX,
    PRC127_IDX,
    PRC255_IDX,
    PRC511_IDX,
    PRC1023_IDX,
    PRC1522_IDX,
    GPRC_IDX,
    BPRC_IDX,
    MPRC_IDX,
    GPTC_IDX,
    GORCL_IDX,
    GORCH_IDX,
    GOTCL_IDX,
    GOTCH_IDX,
    RNBC_IDX,
    RUC_IDX,
    RFC_IDX,
    ROC_IDX,
    RJC_IDX,
    MGTPRC_IDX,
    MGTPDC_IDX,
    MGTPTC_IDX,
    TORL_IDX,
    TORH_IDX,
    TOTL_IDX,
    TOTH_IDX,
    TPR_IDX,
    TPT_IDX,
    PTC64_IDX,
    PTC127_IDX,
    PTC255_IDX,
    PTC511_IDX,
    PTC1023_IDX,
    PTC1522_IDX,
    MPTC_IDX,
    BPTC_IDX,
    TSCTC_IDX,
    TSCTFC_IDX,
    RXCSUM_IDX,
    WUC_IDX,
    WUFC_IDX,
    WUS_IDX,
    MANC_IDX,
    IPAV_IDX,
    WUPL_IDX,
    MTA_IDX,
    RA_IDX,
    VFTA_IDX,
    IP4AT_IDX,
    IP6AT_IDX,
    WUPM_IDX,
    FFLT_IDX,
    FFMT_IDX,
    FFVT_IDX,
    PBM_IDX,
    RA_82542_IDX,
    MTA_82542_IDX,
    VFTA_82542_IDX,
    E1K_NUM_OF_REGS
} E1kRegIndex;

#define E1K_NUM_OF_32BIT_REGS MTA_IDX


/**
 * Define E1000-specific EEPROM layout.
 */
class E1kEEPROM
{
    public:
        EEPROM93C46 eeprom;

#ifdef IN_RING3
        /**
         * Initialize EEPROM content.
         *
         * @param   macAddr     MAC address of E1000.
         */
        void init(RTMAC &macAddr)
        {
            eeprom.init();
            memcpy(eeprom.m_au16Data, macAddr.au16, sizeof(macAddr.au16));
            eeprom.m_au16Data[0x04] = 0xFFFF;
            /*
             * bit 3  - full support for power management
             * bit 10 - full duplex
             */
            eeprom.m_au16Data[0x0A] = 0x4408;
            eeprom.m_au16Data[0x0B] = 0x001E;
            eeprom.m_au16Data[0x0C] = 0x8086;
            eeprom.m_au16Data[0x0D] = 0x100E;
            eeprom.m_au16Data[0x0E] = 0x8086;
            eeprom.m_au16Data[0x0F] = 0x3040;
            eeprom.m_au16Data[0x21] = 0x7061;
            eeprom.m_au16Data[0x22] = 0x280C;
            eeprom.m_au16Data[0x23] = 0x00C8;
            eeprom.m_au16Data[0x24] = 0x00C8;
            eeprom.m_au16Data[0x2F] = 0x0602;
            updateChecksum();
        };

        /**
         * Compute the checksum as required by E1000 and store it
         * in the last word.
         */
        void updateChecksum()
        {
            uint16_t u16Checksum = 0;

            for (int i = 0; i < eeprom.SIZE-1; i++)
                u16Checksum += eeprom.m_au16Data[i];
            eeprom.m_au16Data[eeprom.SIZE-1] = 0xBABA - u16Checksum;
        };

        /**
         * First 6 bytes of EEPROM contain MAC address.
         *
         * @returns MAC address of E1000.
         */
        void getMac(PRTMAC pMac)
        {
            memcpy(pMac->au16, eeprom.m_au16Data, sizeof(pMac->au16));
        };

        uint32_t read()
        {
            return eeprom.read();
        }

        void write(uint32_t u32Wires)
        {
            eeprom.write(u32Wires);
        }

        bool readWord(uint32_t u32Addr, uint16_t *pu16Value)
        {
            return eeprom.readWord(u32Addr, pu16Value);
        }

        int load(PSSMHANDLE pSSM)
        {
            return eeprom.load(pSSM);
        }

        void save(PSSMHANDLE pSSM)
        {
            eeprom.save(pSSM);
        }
#endif /* IN_RING3 */
};


struct E1kRxDStatus
{
    /** @name Descriptor Status field (3.2.3.1)
     * @{ */
    unsigned fDD     : 1;                             /**< Descriptor Done. */
    unsigned fEOP    : 1;                               /**< End of packet. */
    unsigned fIXSM   : 1;                  /**< Ignore checksum indication. */
    unsigned fVP     : 1;                           /**< VLAN, matches VET. */
    unsigned         : 1;
    unsigned fTCPCS  : 1;       /**< RCP Checksum calculated on the packet. */
    unsigned fIPCS   : 1;        /**< IP Checksum calculated on the packet. */
    unsigned fPIF    : 1;                       /**< Passed in-exact filter */
    /** @} */
    /** @name Descriptor Errors field (3.2.3.2)
     * (Only valid when fEOP and fDD are set.)
     * @{ */
    unsigned fCE     : 1;                      /**< CRC or alignment error. */
    unsigned         : 4;    /**< Reserved, varies with different models... */
    unsigned fTCPE   : 1;                      /**< TCP/UDP checksum error. */
    unsigned fIPE    : 1;                           /**< IP Checksum error. */
    unsigned fRXE    : 1;                               /**< RX Data error. */
    /** @} */
    /** @name Descriptor Special field (3.2.3.3)
     * @{  */
    unsigned u12VLAN : 12;                            /**< VLAN identifier. */
    unsigned fCFI    : 1;             /**< Canonical form indicator (VLAN). */
    unsigned u3PRI   : 3;                        /**< User priority (VLAN). */
    /** @} */
};
typedef struct E1kRxDStatus E1KRXDST;

struct E1kRxDesc_st
{
    uint64_t u64BufAddr;                        /**< Address of data buffer */
    uint16_t u16Length;                       /**< Length of data in buffer */
    uint16_t u16Checksum;                              /**< Packet checksum */
    E1KRXDST status;
};
typedef struct E1kRxDesc_st E1KRXDESC;
AssertCompileSize(E1KRXDESC, 16);

#define E1K_DTYP_LEGACY -1
#define E1K_DTYP_CONTEXT 0
#define E1K_DTYP_DATA    1

struct E1kTDLegacy
{
    uint64_t u64BufAddr;                     /**< Address of data buffer */
    struct TDLCmd_st
    {
        unsigned u16Length : 16;
        unsigned u8CSO     : 8;
        /* CMD field       : 8 */
        unsigned fEOP      : 1;
        unsigned fIFCS     : 1;
        unsigned fIC       : 1;
        unsigned fRS       : 1;
        unsigned fRSV      : 1;
        unsigned fDEXT     : 1;
        unsigned fVLE      : 1;
        unsigned fIDE      : 1;
    } cmd;
    struct TDLDw3_st
    {
        /* STA field */
        unsigned fDD       : 1;
        unsigned fEC       : 1;
        unsigned fLC       : 1;
        unsigned fTURSV    : 1;
        /* RSV field */
        unsigned u4RSV     : 4;
        /* CSS field */
        unsigned u8CSS     : 8;
        /* Special field*/
        unsigned u12VLAN   : 12;
        unsigned fCFI      : 1;
        unsigned u3PRI     : 3;
    } dw3;
};

/**
 * TCP/IP Context Transmit Descriptor, section 3.3.6.
 */
struct E1kTDContext
{
    struct CheckSum_st
    {
        /** TSE: Header start. !TSE: Checksum start. */
        unsigned u8CSS     : 8;
        /** Checksum offset - where to store it. */
        unsigned u8CSO     : 8;
        /** Checksum ending (inclusive) offset, 0 = end of packet. */
        unsigned u16CSE    : 16;
    } ip;
    struct CheckSum_st tu;
    struct TDCDw2_st
    {
        /** TSE: The total number of payload bytes for this context. Sans header. */
        unsigned u20PAYLEN : 20;
        /** The descriptor type - E1K_DTYP_CONTEXT (0). */
        unsigned u4DTYP    : 4;
        /** TUCMD field, 8 bits
         * @{ */
        /** TSE: TCP (set) or UDP (clear). */
        unsigned fTCP      : 1;
        /** TSE: IPv4 (set) or IPv6 (clear) - for finding the payload length field in
         * the IP header.  Does not affect the checksumming.
         * @remarks 82544GC/EI interprets a cleared field differently.  */
        unsigned fIP       : 1;
        /** TSE: TCP segmentation enable.  When clear the context describes  */
        unsigned fTSE      : 1;
        /** Report status (only applies to dw3.fDD for here). */
        unsigned fRS       : 1;
        /** Reserved, MBZ. */
        unsigned fRSV1     : 1;
        /** Descriptor extension, must be set for this descriptor type. */
        unsigned fDEXT     : 1;
        /** Reserved, MBZ. */
        unsigned fRSV2     : 1;
        /** Interrupt delay enable. */
        unsigned fIDE      : 1;
        /** @} */
    } dw2;
    struct TDCDw3_st
    {
        /** Descriptor Done. */
        unsigned fDD       : 1;
        /** Reserved, MBZ. */
        unsigned u7RSV     : 7;
        /** TSO: The header (prototype) length (Ethernet[, VLAN tag], IP, TCP/UDP. */
        unsigned u8HDRLEN  : 8;
        /** TSO: Maximum segment size. */
        unsigned u16MSS    : 16;
    } dw3;
};
typedef struct E1kTDContext E1KTXCTX;

/**
 * TCP/IP Data Transmit Descriptor, section 3.3.7.
 */
struct E1kTDData
{
    uint64_t u64BufAddr;                        /**< Address of data buffer */
    struct TDDCmd_st
    {
        /** The total length of data pointed to by this descriptor. */
        unsigned u20DTALEN : 20;
        /** The descriptor type - E1K_DTYP_DATA (1). */
        unsigned u4DTYP    : 4;
        /** @name DCMD field, 8 bits (3.3.7.1).
         * @{ */
        /** End of packet.  Note TSCTFC update.  */
        unsigned fEOP      : 1;
        /** Insert Ethernet FCS/CRC (requires fEOP to be set). */
        unsigned fIFCS     : 1;
        /** Use the TSE context when set and the normal when clear. */
        unsigned fTSE      : 1;
        /** Report status (dw3.STA). */
        unsigned fRS       : 1;
        /** Reserved. 82544GC/EI defines this report packet set (RPS).  */
        unsigned fRSV      : 1;
        /** Descriptor extension, must be set for this descriptor type. */
        unsigned fDEXT     : 1;
        /** VLAN enable, requires CTRL.VME, auto enables FCS/CRC.
         *  Insert dw3.SPECIAL after ethernet header. */
        unsigned fVLE      : 1;
        /** Interrupt delay enable. */
        unsigned fIDE      : 1;
        /** @} */
    } cmd;
    struct TDDDw3_st
    {
        /** @name STA field (3.3.7.2)
         * @{  */
        unsigned fDD       : 1;                       /**< Descriptor done. */
        unsigned fEC       : 1;                      /**< Excess collision. */
        unsigned fLC       : 1;                        /**< Late collision. */
        /** Reserved, except for the usual oddball (82544GC/EI) where it's called TU. */
        unsigned fTURSV    : 1;
        /** @} */
        unsigned u4RSV     : 4;                   /**< Reserved field, MBZ. */
        /** @name POPTS (Packet Option) field (3.3.7.3)
         * @{  */
        unsigned fIXSM     : 1;                    /**< Insert IP checksum. */
        unsigned fTXSM     : 1;               /**< Insert TCP/UDP checksum. */
        unsigned u6RSV     : 6;                         /**< Reserved, MBZ. */
        /** @} */
        /** @name SPECIAL field - VLAN tag to be inserted after ethernet header.
         * Requires fEOP, fVLE and CTRL.VME to be set.
         * @{ */
        unsigned u12VLAN   : 12;                      /**< VLAN identifier. */
        unsigned fCFI      : 1;       /**< Canonical form indicator (VLAN). */
        unsigned u3PRI     : 3;                  /**< User priority (VLAN). */
        /** @}  */
    } dw3;
};
typedef struct E1kTDData E1KTXDAT;

union E1kTxDesc
{
    struct E1kTDLegacy  legacy;
    struct E1kTDContext context;
    struct E1kTDData    data;
};
typedef union  E1kTxDesc E1KTXDESC;
AssertCompileSize(E1KTXDESC, 16);

#define RA_CTL_AS 0x0003
#define RA_CTL_AV 0x8000

union E1kRecAddr
{
    uint32_t au32[32];
    struct RAArray
    {
        uint8_t  addr[6];
        uint16_t ctl;
    } array[16];
};
typedef struct E1kRecAddr::RAArray E1KRAELEM;
typedef union E1kRecAddr E1KRA;
AssertCompileSize(E1KRA, 8*16);

#define E1K_IP_RF 0x8000        /* reserved fragment flag */
#define E1K_IP_DF 0x4000        /* dont fragment flag */
#define E1K_IP_MF 0x2000        /* more fragments flag */
#define E1K_IP_OFFMASK 0x1fff   /* mask for fragmenting bits */

/** @todo use+extend RTNETIPV4 */
struct E1kIpHeader
{
    /* type of service / version / header length */
    uint16_t tos_ver_hl;
    /* total length */
    uint16_t total_len;
    /* identification */
    uint16_t ident;
    /* fragment offset field */
    uint16_t offset;
    /* time to live / protocol*/
    uint16_t ttl_proto;
    /* checksum */
    uint16_t chksum;
    /* source IP address */
    uint32_t src;
    /* destination IP address */
    uint32_t dest;
};
AssertCompileSize(struct E1kIpHeader, 20);

#define E1K_TCP_FIN 0x01U
#define E1K_TCP_SYN 0x02U
#define E1K_TCP_RST 0x04U
#define E1K_TCP_PSH 0x08U
#define E1K_TCP_ACK 0x10U
#define E1K_TCP_URG 0x20U
#define E1K_TCP_ECE 0x40U
#define E1K_TCP_CWR 0x80U

#define E1K_TCP_FLAGS 0x3fU

/** @todo use+extend RTNETTCP */
struct E1kTcpHeader
{
    uint16_t src;
    uint16_t dest;
    uint32_t seqno;
    uint32_t ackno;
    uint16_t hdrlen_flags;
    uint16_t wnd;
    uint16_t chksum;
    uint16_t urgp;
};
AssertCompileSize(struct E1kTcpHeader, 20);


/** The current Saved state version. */
#define E1K_SAVEDSTATE_VERSION          2
/** Saved state version for VirtualBox 3.0 and earlier.
 * This did not include the configuration part nor the E1kEEPROM.  */
#define E1K_SAVEDSTATE_VERSION_VBOX_30  1

/**
 * Device state structure. Holds the current state of device.
 *
 * @implements  PDMINETWORKDOWN
 * @implements  PDMINETWORKCONFIG
 * @implements  PDMILEDPORTS
 */
struct E1kState_st
{
    char                    szInstance[8];        /**< Instance name, e.g. E1000#1. */
    PDMIBASE                IBase;
    PDMINETWORKDOWN         INetworkDown;
    PDMINETWORKCONFIG       INetworkConfig;
    PDMILEDPORTS            ILeds;                               /**< LED interface */
    R3PTRTYPE(PPDMIBASE)    pDrvBase;                 /**< Attached network driver. */
    R3PTRTYPE(PPDMILEDCONNECTORS)    pLedsConnector;

    PPDMDEVINSR3            pDevInsR3;                   /**< Device instance - R3. */
    R3PTRTYPE(PPDMQUEUE)    pTxQueueR3;                   /**< Transmit queue - R3. */
    R3PTRTYPE(PPDMQUEUE)    pCanRxQueueR3;           /**< Rx wakeup signaller - R3. */
    PPDMINETWORKUPR3        pDrvR3;              /**< Attached network driver - R3. */
    PTMTIMERR3              pRIDTimerR3;   /**< Receive Interrupt Delay Timer - R3. */
    PTMTIMERR3              pRADTimerR3;    /**< Receive Absolute Delay Timer - R3. */
    PTMTIMERR3              pTIDTimerR3;  /**< Transmit Interrupt Delay Timer - R3. */
    PTMTIMERR3              pTADTimerR3;   /**< Transmit Absolute Delay Timer - R3. */
    PTMTIMERR3              pIntTimerR3;            /**< Late Interrupt Timer - R3. */
    PTMTIMERR3              pLUTimerR3;               /**< Link Up(/Restore) Timer. */
    /** The scatter / gather buffer used for the current outgoing packet - R3. */
    R3PTRTYPE(PPDMSCATTERGATHER) pTxSgR3;

    PPDMDEVINSR0            pDevInsR0;                   /**< Device instance - R0. */
    R0PTRTYPE(PPDMQUEUE)    pTxQueueR0;                   /**< Transmit queue - R0. */
    R0PTRTYPE(PPDMQUEUE)    pCanRxQueueR0;           /**< Rx wakeup signaller - R0. */
    PPDMINETWORKUPR0        pDrvR0;              /**< Attached network driver - R0. */
    PTMTIMERR0              pRIDTimerR0;   /**< Receive Interrupt Delay Timer - R0. */
    PTMTIMERR0              pRADTimerR0;    /**< Receive Absolute Delay Timer - R0. */
    PTMTIMERR0              pTIDTimerR0;  /**< Transmit Interrupt Delay Timer - R0. */
    PTMTIMERR0              pTADTimerR0;   /**< Transmit Absolute Delay Timer - R0. */
    PTMTIMERR0              pIntTimerR0;            /**< Late Interrupt Timer - R0. */
    PTMTIMERR0              pLUTimerR0;          /**< Link Up(/Restore) Timer - R0. */
    /** The scatter / gather buffer used for the current outgoing packet - R0. */
    R0PTRTYPE(PPDMSCATTERGATHER) pTxSgR0;

    PPDMDEVINSRC            pDevInsRC;                   /**< Device instance - RC. */
    RCPTRTYPE(PPDMQUEUE)    pTxQueueRC;                   /**< Transmit queue - RC. */
    RCPTRTYPE(PPDMQUEUE)    pCanRxQueueRC;           /**< Rx wakeup signaller - RC. */
    PPDMINETWORKUPRC        pDrvRC;              /**< Attached network driver - RC. */
    PTMTIMERRC              pRIDTimerRC;   /**< Receive Interrupt Delay Timer - RC. */
    PTMTIMERRC              pRADTimerRC;    /**< Receive Absolute Delay Timer - RC. */
    PTMTIMERRC              pTIDTimerRC;  /**< Transmit Interrupt Delay Timer - RC. */
    PTMTIMERRC              pTADTimerRC;   /**< Transmit Absolute Delay Timer - RC. */
    PTMTIMERRC              pIntTimerRC;            /**< Late Interrupt Timer - RC. */
    PTMTIMERRC              pLUTimerRC;          /**< Link Up(/Restore) Timer - RC. */
    /** The scatter / gather buffer used for the current outgoing packet - RC. */
    RCPTRTYPE(PPDMSCATTERGATHER) pTxSgRC;
    RTRCPTR                 RCPtrAlignment;

#if HC_ARCH_BITS == 32
    uint32_t                Alignment1;
#endif
    PDMCRITSECT cs;                  /**< Critical section - what is it protecting? */
#ifndef E1K_GLOBAL_MUTEX
    PDMCRITSECT csRx;                                     /**< RX Critical section. */
//    PDMCRITSECT csTx;                                     /**< TX Critical section. */
#endif
    /** Base address of memory-mapped registers. */
    RTGCPHYS    addrMMReg;
    /** MAC address obtained from the configuration. */
    RTMAC       macConfigured;
    /** Base port of I/O space region. */
    RTIOPORT    addrIOPort;
    /** EMT: */
    PCIDEVICE   pciDevice;
    /** EMT: Last time the interrupt was acknowledged.  */
    uint64_t    u64AckedAt;
    /** All: Used for eliminating spurious interrupts. */
    bool        fIntRaised;
    /** EMT: false if the cable is disconnected by the GUI. */
    bool        fCableConnected;
    /** EMT: */
    bool        fR0Enabled;
    /** EMT: */
    bool        fGCEnabled;

    /** All: Device register storage. */
    uint32_t    auRegs[E1K_NUM_OF_32BIT_REGS];
    /** TX/RX: Status LED. */
    PDMLED      led;
    /** TX/RX: Number of packet being sent/received to show in debug log. */
    uint32_t    u32PktNo;

    /** EMT: Offset of the register to be read via IO. */
    uint32_t    uSelectedReg;
    /** EMT: Multicast Table Array. */
    uint32_t    auMTA[128];
    /** EMT: Receive Address registers.  */
    E1KRA       aRecAddr;
    /** EMT: VLAN filter table array. */
    uint32_t    auVFTA[128];
    /** EMT: Receive buffer size. */
    uint16_t    u16RxBSize;
    /** EMT: Locked state -- no state alteration possible. */
    bool        fLocked;
    /** EMT: */
    bool        fDelayInts;
    /** All: */
    bool        fIntMaskUsed;

    /** N/A: */
    bool volatile fMaybeOutOfSpace;
    /** EMT: Gets signalled when more RX descriptors become available. */
    RTSEMEVENT  hEventMoreRxDescAvail;

    /** TX: Context used for TCP segmentation packets. */
    E1KTXCTX    contextTSE;
    /** TX: Context used for ordinary packets. */
    E1KTXCTX    contextNormal;
    /** GSO context. u8Type is set to PDMNETWORKGSOTYPE_INVALID when not
     *  applicable to the current TSE mode. */
    PDMNETWORKGSO GsoCtx;
    /** Scratch space for holding the loopback / fallback scatter / gather
     *  descriptor. */
    union
    {
        PDMSCATTERGATHER    Sg;
        uint8_t             padding[8 * sizeof(RTUINTPTR)];
    }           uTxFallback;
    /** TX: Transmit packet buffer use for TSE fallback and loopback. */
    uint8_t     aTxPacketFallback[E1K_MAX_TX_PKT_SIZE];
    /** TX: Number of bytes assembled in TX packet buffer. */
    uint16_t    u16TxPktLen;
    /** TX: IP checksum has to be inserted if true. */
    bool        fIPcsum;
    /** TX: TCP/UDP checksum has to be inserted if true. */
    bool        fTCPcsum;
    /** TX TSE fallback: Number of payload bytes remaining in TSE context. */
    uint32_t    u32PayRemain;
    /** TX TSE fallback: Number of header bytes remaining in TSE context. */
    uint16_t    u16HdrRemain;
    /** TX TSE fallback: Flags from template header. */
    uint16_t    u16SavedFlags;
    /** TX TSE fallback: Partial checksum from template header. */
    uint32_t    u32SavedCsum;
    /** ?: Emulated controller type. */
    E1KCHIP     eChip;
    uint32_t    alignmentFix;

    /** EMT: EEPROM emulation */
    E1kEEPROM   eeprom;
    /** EMT: Physical interface emulation. */
    PHY         phy;

#if 0
    /** Alignment padding. */
    uint8_t                             Alignment[HC_ARCH_BITS == 64 ? 8 : 4];
#endif

    STAMCOUNTER                         StatReceiveBytes;
    STAMCOUNTER                         StatTransmitBytes;
#if defined(VBOX_WITH_STATISTICS) || defined(E1K_REL_STATS)
    STAMPROFILEADV                      StatMMIOReadRZ;
    STAMPROFILEADV                      StatMMIOReadR3;
    STAMPROFILEADV                      StatMMIOWriteRZ;
    STAMPROFILEADV                      StatMMIOWriteR3;
    STAMPROFILEADV                      StatEEPROMRead;
    STAMPROFILEADV                      StatEEPROMWrite;
    STAMPROFILEADV                      StatIOReadRZ;
    STAMPROFILEADV                      StatIOReadR3;
    STAMPROFILEADV                      StatIOWriteRZ;
    STAMPROFILEADV                      StatIOWriteR3;
    STAMPROFILEADV                      StatLateIntTimer;
    STAMCOUNTER                         StatLateInts;
    STAMCOUNTER                         StatIntsRaised;
    STAMCOUNTER                         StatIntsPrevented;
    STAMPROFILEADV                      StatReceive;
    STAMPROFILEADV                      StatReceiveFilter;
    STAMPROFILEADV                      StatReceiveStore;
    STAMPROFILEADV                      StatTransmitRZ;
    STAMPROFILEADV                      StatTransmitR3;
    STAMPROFILE                         StatTransmitSendRZ;
    STAMPROFILE                         StatTransmitSendR3;
    STAMPROFILE                         StatRxOverflow;
    STAMCOUNTER                         StatRxOverflowWakeup;
    STAMCOUNTER                         StatTxDescCtxNormal;
    STAMCOUNTER                         StatTxDescCtxTSE;
    STAMCOUNTER                         StatTxDescLegacy;
    STAMCOUNTER                         StatTxDescData;
    STAMCOUNTER                         StatTxDescTSEData;
    STAMCOUNTER                         StatTxPathFallback;
    STAMCOUNTER                         StatTxPathGSO;
    STAMCOUNTER                         StatTxPathRegular;
    STAMCOUNTER                         StatPHYAccesses;

#endif /* VBOX_WITH_STATISTICS || E1K_REL_STATS */

#ifdef E1K_INT_STATS
    /* Internal stats */
    uint32_t    uStatInt;
    uint32_t    uStatIntTry;
    int32_t     uStatIntLower;
    uint32_t    uStatIntDly;
    int32_t     iStatIntLost;
    int32_t     iStatIntLostOne;
    uint32_t    uStatDisDly;
    uint32_t    uStatIntSkip;
    uint32_t    uStatIntLate;
    uint32_t    uStatIntMasked;
    uint32_t    uStatIntEarly;
    uint32_t    uStatIntRx;
    uint32_t    uStatIntTx;
    uint32_t    uStatIntICS;
    uint32_t    uStatIntRDTR;
    uint32_t    uStatIntRXDMT0;
    uint32_t    uStatIntTXQE;
    uint32_t    uStatTxNoRS;
    uint32_t    uStatTxIDE;
    uint32_t    uStatTAD;
    uint32_t    uStatTID;
    uint32_t    uStatRAD;
    uint32_t    uStatRID;
    uint32_t    uStatRxFrm;
    uint32_t    uStatTxFrm;
    uint32_t    uStatDescCtx;
    uint32_t    uStatDescDat;
    uint32_t    uStatDescLeg;
#endif /* E1K_INT_STATS */
};
typedef struct E1kState_st E1KSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

/* Forward declarations ******************************************************/
static int e1kXmitPending(E1KSTATE *pState, bool fOnWorkerThread);

static int e1kRegReadUnimplemented (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteUnimplemented(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadAutoClear     (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegReadDefault       (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteDefault      (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
#if 0 /* unused */
static int e1kRegReadCTRL          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
#endif
static int e1kRegWriteCTRL         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadEECD          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteEECD         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteEERD         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteMDIC         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadICR           (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteICR          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteICS          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteIMS          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteIMC          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteRCTL         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWritePBA          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteRDT          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteRDTR         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegWriteTDT          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadMTA           (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteMTA          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadRA            (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteRA           (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
static int e1kRegReadVFTA          (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
static int e1kRegWriteVFTA         (E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);

/**
 * Register map table.
 *
 * Override fn_read and fn_write to get register-specific behavior.
 */
const static struct E1kRegMap_st
{
    /** Register offset in the register space. */
    uint32_t   offset;
    /** Size in bytes. Registers of size > 4 are in fact tables. */
    uint32_t   size;
    /** Readable bits. */
    uint32_t   readable;
    /** Writable bits. */
    uint32_t   writable;
    /** Read callback. */
    int       (*pfnRead)(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value);
    /** Write callback. */
    int       (*pfnWrite)(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t u32Value);
    /** Abbreviated name. */
    const char *abbrev;
    /** Full name. */
    const char *name;
} s_e1kRegMap[E1K_NUM_OF_REGS] =
{
    /* offset  size     read mask   write mask  read callback            write callback            abbrev      full name                     */
    /*-------  -------  ----------  ----------  -----------------------  ------------------------  ----------  ------------------------------*/
    { 0x00000, 0x00004, 0xDBF31BE9, 0xDBF31BE9, e1kRegReadDefault      , e1kRegWriteCTRL         , "CTRL"    , "Device Control" },
    { 0x00008, 0x00004, 0x0000FDFF, 0x00000000, e1kRegReadDefault      , e1kRegWriteUnimplemented, "STATUS"  , "Device Status" },
    { 0x00010, 0x00004, 0x000027F0, 0x00000070, e1kRegReadEECD         , e1kRegWriteEECD         , "EECD"    , "EEPROM/Flash Control/Data" },
    { 0x00014, 0x00004, 0xFFFFFF10, 0xFFFFFF00, e1kRegReadDefault      , e1kRegWriteEERD         , "EERD"    , "EEPROM Read" },
    { 0x00018, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "CTRL_EXT", "Extended Device Control" },
    { 0x0001c, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FLA"     , "Flash Access (N/A)" },
    { 0x00020, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteMDIC         , "MDIC"    , "MDI Control" },
    { 0x00028, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCAL"    , "Flow Control Address Low" },
    { 0x0002c, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCAH"    , "Flow Control Address High" },
    { 0x00030, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCT"     , "Flow Control Type" },
    { 0x00038, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "VET"     , "VLAN EtherType" },
    { 0x000c0, 0x00004, 0x0001F6DF, 0x0001F6DF, e1kRegReadICR          , e1kRegWriteICR          , "ICR"     , "Interrupt Cause Read" },
    { 0x000c4, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "ITR"     , "Interrupt Throttling" },
    { 0x000c8, 0x00004, 0x00000000, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteICS          , "ICS"     , "Interrupt Cause Set" },
    { 0x000d0, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteIMS          , "IMS"     , "Interrupt Mask Set/Read" },
    { 0x000d8, 0x00004, 0x00000000, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteIMC          , "IMC"     , "Interrupt Mask Clear" },
    { 0x00100, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteRCTL         , "RCTL"    , "Receive Control" },
    { 0x00170, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCTTV"   , "Flow Control Transmit Timer Value" },
    { 0x00178, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TXCW"    , "Transmit Configuration Word (N/A)" },
    { 0x00180, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RXCW"    , "Receive Configuration Word (N/A)" },
    { 0x00400, 0x00004, 0x017FFFFA, 0x017FFFFA, e1kRegReadDefault      , e1kRegWriteDefault      , "TCTL"    , "Transmit Control" },
    { 0x00410, 0x00004, 0x3FFFFFFF, 0x3FFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TIPG"    , "Transmit IPG" },
    { 0x00458, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "AIFS"    , "Adaptive IFS Throttle - AIT" },
    { 0x00e00, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "LEDCTL"  , "LED Control" },
    { 0x01000, 0x00004, 0xFFFF007F, 0x0000007F, e1kRegReadDefault      , e1kRegWritePBA          , "PBA"     , "Packet Buffer Allocation" },
    { 0x02160, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCRTL"   , "Flow Control Receive Threshold Low" },
    { 0x02168, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCRTH"   , "Flow Control Receive Threshold High" },
    { 0x02410, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RDFH"    , "Receive Data FIFO Head" },
    { 0x02418, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RDFT"    , "Receive Data FIFO Tail" },
    { 0x02420, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RDFHS"   , "Receive Data FIFO Head Saved Register" },
    { 0x02428, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RDFTS"   , "Receive Data FIFO Tail Saved Register" },
    { 0x02430, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RDFPC"   , "Receive Data FIFO Packet Count" },
    { 0x02800, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "RDBAL"   , "Receive Descriptor Base Low" },
    { 0x02804, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "RDBAH"   , "Receive Descriptor Base High" },
    { 0x02808, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "RDLEN"   , "Receive Descriptor Length" },
    { 0x02810, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "RDH"     , "Receive Descriptor Head" },
    { 0x02818, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteRDT          , "RDT"     , "Receive Descriptor Tail" },
    { 0x02820, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteRDTR         , "RDTR"    , "Receive Delay Timer" },
    { 0x02828, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RXDCTL"  , "Receive Descriptor Control" },
    { 0x0282c, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "RADV"    , "Receive Interrupt Absolute Delay Timer" },
    { 0x02c00, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RSRPD"   , "Receive Small Packet Detect Interrupt" },
    { 0x03000, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TXDMAC"  , "TX DMA Control (N/A)" },
    { 0x03410, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TDFH"    , "Transmit Data FIFO Head" },
    { 0x03418, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TDFT"    , "Transmit Data FIFO Tail" },
    { 0x03420, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TDFHS"   , "Transmit Data FIFO Head Saved Register" },
    { 0x03428, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TDFTS"   , "Transmit Data FIFO Tail Saved Register" },
    { 0x03430, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TDFPC"   , "Transmit Data FIFO Packet Count" },
    { 0x03800, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TDBAL"   , "Transmit Descriptor Base Low" },
    { 0x03804, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TDBAH"   , "Transmit Descriptor Base High" },
    { 0x03808, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TDLEN"   , "Transmit Descriptor Length" },
    { 0x03810, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TDH"     , "Transmit Descriptor Head" },
    { 0x03818, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteTDT          , "TDT"     , "Transmit Descriptor Tail" },
    { 0x03820, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TIDV"    , "Transmit Interrupt Delay Value" },
    { 0x03828, 0x00004, 0xFF3F3F3F, 0xFF3F3F3F, e1kRegReadDefault      , e1kRegWriteDefault      , "TXDCTL"  , "Transmit Descriptor Control" },
    { 0x0382c, 0x00004, 0x0000FFFF, 0x0000FFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TADV"    , "Transmit Absolute Interrupt Delay Timer" },
    { 0x03830, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "TSPMT"   , "TCP Segmentation Pad and Threshold" },
    { 0x04000, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "CRCERRS" , "CRC Error Count" },
    { 0x04004, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "ALGNERRC", "Alignment Error Count" },
    { 0x04008, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "SYMERRS" , "Symbol Error Count" },
    { 0x0400c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RXERRC"  , "RX Error Count" },
    { 0x04010, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "MPC"     , "Missed Packets Count" },
    { 0x04014, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "SCC"     , "Single Collision Count" },
    { 0x04018, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "ECOL"    , "Excessive Collisions Count" },
    { 0x0401c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "MCC"     , "Multiple Collision Count" },
    { 0x04020, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "LATECOL" , "Late Collisions Count" },
    { 0x04028, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "COLC"    , "Collision Count" },
    { 0x04030, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "DC"      , "Defer Count" },
    { 0x04034, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "TNCRS"   , "Transmit - No CRS" },
    { 0x04038, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "SEC"     , "Sequence Error Count" },
    { 0x0403c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "CEXTERR" , "Carrier Extension Error Count" },
    { 0x04040, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RLEC"    , "Receive Length Error Count" },
    { 0x04048, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "XONRXC"  , "XON Received Count" },
    { 0x0404c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "XONTXC"  , "XON Transmitted Count" },
    { 0x04050, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "XOFFRXC" , "XOFF Received Count" },
    { 0x04054, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "XOFFTXC" , "XOFF Transmitted Count" },
    { 0x04058, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FCRUC"   , "FC Received Unsupported Count" },
    { 0x0405c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC64"   , "Packets Received (64 Bytes) Count" },
    { 0x04060, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC127"  , "Packets Received (65-127 Bytes) Count" },
    { 0x04064, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC255"  , "Packets Received (128-255 Bytes) Count" },
    { 0x04068, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC511"  , "Packets Received (256-511 Bytes) Count" },
    { 0x0406c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC1023" , "Packets Received (512-1023 Bytes) Count" },
    { 0x04070, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PRC1522" , "Packets Received (1024-Max Bytes)" },
    { 0x04074, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GPRC"    , "Good Packets Received Count" },
    { 0x04078, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "BPRC"    , "Broadcast Packets Received Count" },
    { 0x0407c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "MPRC"    , "Multicast Packets Received Count" },
    { 0x04080, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GPTC"    , "Good Packets Transmitted Count" },
    { 0x04088, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GORCL"   , "Good Octets Received Count (Low)" },
    { 0x0408c, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GORCH"   , "Good Octets Received Count (Hi)" },
    { 0x04090, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GOTCL"   , "Good Octets Transmitted Count (Low)" },
    { 0x04094, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "GOTCH"   , "Good Octets Transmitted Count (Hi)" },
    { 0x040a0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RNBC"    , "Receive No Buffers Count" },
    { 0x040a4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RUC"     , "Receive Undersize Count" },
    { 0x040a8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RFC"     , "Receive Fragment Count" },
    { 0x040ac, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "ROC"     , "Receive Oversize Count" },
    { 0x040b0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "RJC"     , "Receive Jabber Count" },
    { 0x040b4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "MGTPRC"  , "Management Packets Received Count" },
    { 0x040b8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "MGTPDC"  , "Management Packets Dropped Count" },
    { 0x040bc, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "MGTPTC"  , "Management Pkts Transmitted Count" },
    { 0x040c0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TORL"    , "Total Octets Received (Lo)" },
    { 0x040c4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TORH"    , "Total Octets Received (Hi)" },
    { 0x040c8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TOTL"    , "Total Octets Transmitted (Lo)" },
    { 0x040cc, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TOTH"    , "Total Octets Transmitted (Hi)" },
    { 0x040d0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TPR"     , "Total Packets Received" },
    { 0x040d4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TPT"     , "Total Packets Transmitted" },
    { 0x040d8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC64"   , "Packets Transmitted (64 Bytes) Count" },
    { 0x040dc, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC127"  , "Packets Transmitted (65-127 Bytes) Count" },
    { 0x040e0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC255"  , "Packets Transmitted (128-255 Bytes) Count" },
    { 0x040e4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC511"  , "Packets Transmitted (256-511 Bytes) Count" },
    { 0x040e8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC1023" , "Packets Transmitted (512-1023 Bytes) Count" },
    { 0x040ec, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "PTC1522" , "Packets Transmitted (1024 Bytes or Greater) Count" },
    { 0x040f0, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "MPTC"    , "Multicast Packets Transmitted Count" },
    { 0x040f4, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "BPTC"    , "Broadcast Packets Transmitted Count" },
    { 0x040f8, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TSCTC"   , "TCP Segmentation Context Transmitted Count" },
    { 0x040fc, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadAutoClear    , e1kRegWriteUnimplemented, "TSCTFC"  , "TCP Segmentation Context Tx Fail Count" },
    { 0x05000, 0x00004, 0x000007FF, 0x000007FF, e1kRegReadDefault      , e1kRegWriteDefault      , "RXCSUM"  , "Receive Checksum Control" },
    { 0x05800, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "WUC"     , "Wakeup Control" },
    { 0x05808, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "WUFC"    , "Wakeup Filter Control" },
    { 0x05810, 0x00004, 0xFFFFFFFF, 0x00000000, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "WUS"     , "Wakeup Status" },
    { 0x05820, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadDefault      , e1kRegWriteDefault      , "MANC"    , "Management Control" },
    { 0x05838, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "IPAV"    , "IP Address Valid" },
    { 0x05900, 0x00004, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "WUPL"    , "Wakeup Packet Length" },
    { 0x05200, 0x00200, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadMTA          , e1kRegWriteMTA          , "MTA"     , "Multicast Table Array (n)" },
    { 0x05400, 0x00080, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadRA           , e1kRegWriteRA           , "RA"      , "Receive Address (64-bit) (n)" },
    { 0x05600, 0x00200, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadVFTA         , e1kRegWriteVFTA         , "VFTA"    , "VLAN Filter Table Array (n)" },
    { 0x05840, 0x0001c, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "IP4AT"   , "IPv4 Address Table" },
    { 0x05880, 0x00010, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "IP6AT"   , "IPv6 Address Table" },
    { 0x05a00, 0x00080, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "WUPM"    , "Wakeup Packet Memory" },
    { 0x05f00, 0x0001c, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FFLT"    , "Flexible Filter Length Table" },
    { 0x09000, 0x003fc, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FFMT"    , "Flexible Filter Mask Table" },
    { 0x09800, 0x003fc, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "FFVT"    , "Flexible Filter Value Table" },
    { 0x10000, 0x10000, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadUnimplemented, e1kRegWriteUnimplemented, "PBM"     , "Packet Buffer Memory (n)" },
    { 0x00040, 0x00080, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadRA           , e1kRegWriteRA           , "RA"      , "Receive Address (64-bit) (n) (82542)" },
    { 0x00200, 0x00200, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadMTA          , e1kRegWriteMTA          , "MTA"     , "Multicast Table Array (n) (82542)" },
    { 0x00600, 0x00200, 0xFFFFFFFF, 0xFFFFFFFF, e1kRegReadVFTA         , e1kRegWriteVFTA         , "VFTA"    , "VLAN Filter Table Array (n) (82542)" }
};

#ifdef DEBUG

/**
 * Convert U32 value to hex string. Masked bytes are replaced with dots.
 *
 * @remarks The mask has byte (not bit) granularity (e.g. 000000FF).
 *
 * @returns The buffer.
 *
 * @param   u32         The word to convert into string.
 * @param   mask        Selects which bytes to convert.
 * @param   buf         Where to put the result.
 */
static char *e1kU32toHex(uint32_t u32, uint32_t mask, char *buf)
{
    for (char *ptr = buf + 7; ptr >= buf; --ptr, u32 >>=4, mask >>=4)
    {
        if (mask & 0xF)
            *ptr = (u32 & 0xF) + ((u32 & 0xF) > 9 ? '7' : '0');
        else
            *ptr = '.';
    }
    buf[8] = 0;
    return buf;
}

/**
 * Returns timer name for debug purposes.
 *
 * @returns The timer name.
 *
 * @param   pState      The device state structure.
 * @param   pTimer      The timer to get the name for.
 */
DECLINLINE(const char *) e1kGetTimerName(E1KSTATE *pState, PTMTIMER pTimer)
{
    if (pTimer == pState->CTX_SUFF(pTIDTimer))
        return "TID";
    if (pTimer == pState->CTX_SUFF(pTADTimer))
        return "TAD";
    if (pTimer == pState->CTX_SUFF(pRIDTimer))
        return "RID";
    if (pTimer == pState->CTX_SUFF(pRADTimer))
        return "RAD";
    if (pTimer == pState->CTX_SUFF(pIntTimer))
        return "Int";
    return "unknown";
}

#endif /* DEBUG */

/**
 * Arm a timer.
 *
 * @param   pState      Pointer to the device state structure.
 * @param   pTimer      Pointer to the timer.
 * @param   uExpireIn   Expiration interval in microseconds.
 */
DECLINLINE(void) e1kArmTimer(E1KSTATE *pState, PTMTIMER pTimer, uint32_t uExpireIn)
{
    if (pState->fLocked)
        return;

    E1kLog2(("%s Arming %s timer to fire in %d usec...\n",
             INSTANCE(pState), e1kGetTimerName(pState, pTimer), uExpireIn));
    TMTimerSet(pTimer, TMTimerFromMicro(pTimer, uExpireIn) +
            TMTimerGet(pTimer));
}

/**
 * Cancel a timer.
 *
 * @param   pState      Pointer to the device state structure.
 * @param   pTimer      Pointer to the timer.
 */
DECLINLINE(void) e1kCancelTimer(E1KSTATE *pState, PTMTIMER pTimer)
{
    E1kLog2(("%s Stopping %s timer...\n",
            INSTANCE(pState), e1kGetTimerName(pState, pTimer)));
    int rc = TMTimerStop(pTimer);
    if (RT_FAILURE(rc))
    {
        E1kLog2(("%s e1kCancelTimer: TMTimerStop() failed with %Rrc\n",
                INSTANCE(pState), rc));
    }
}

#ifdef E1K_GLOBAL_MUTEX

DECLINLINE(int) e1kCsEnter(E1KSTATE *pState, int iBusyRc)
{
    return VINF_SUCCESS;
}

DECLINLINE(void) e1kCsLeave(E1KSTATE *pState)
{
}

# define e1kCsRxEnter(ps, rc) VINF_SUCCESS
# define e1kCsRxLeave(ps) do { } while (0)

# define e1kCsTxEnter(ps, rc) VINF_SUCCESS
# define e1kCsTxLeave(ps) do { } while (0)


DECLINLINE(int) e1kMutexAcquire(E1KSTATE *pState, int iBusyRc, RT_SRC_POS_DECL)
{
    int rc = PDMCritSectEnter(&pState->cs, iBusyRc);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
    {
        E1kLog2(("%s ==> FAILED to enter critical section at %s:%d:%s with rc=\n",
                INSTANCE(pState), RT_SRC_POS_ARGS, rc));
        PDMDevHlpDBGFStop(pState->CTX_SUFF(pDevIns), RT_SRC_POS_ARGS,
                          "%s Failed to enter critical section, rc=%Rrc\n",
                          INSTANCE(pState), rc);
    }
    else
    {
        //E1kLog2(("%s ==> Mutex acquired at %s:%d:%s\n", INSTANCE(pState), RT_SRC_POS_ARGS));
    }
    return rc;
}

DECLINLINE(void) e1kMutexRelease(E1KSTATE *pState)
{
    //E1kLog2(("%s <== Releasing mutex...\n", INSTANCE(pState)));
    PDMCritSectLeave(&pState->cs);
}

#else /* !E1K_GLOBAL_MUTEX */
# define e1kCsEnter(ps, rc) PDMCritSectEnter(&ps->cs, rc)
# define e1kCsLeave(ps) PDMCritSectLeave(&ps->cs)

# define e1kCsRxEnter(ps, rc) PDMCritSectEnter(&ps->csRx, rc)
# define e1kCsRxLeave(ps) PDMCritSectLeave(&ps->csRx)

# define e1kCsTxEnter(ps, rc) VINF_SUCCESS
# define e1kCsTxLeave(ps) do { } while (0)
//# define e1kCsTxEnter(ps, rc) PDMCritSectEnter(&ps->csTx, rc)
//# define e1kCsTxLeave(ps) PDMCritSectLeave(&ps->csTx)

# if 0
DECLINLINE(int) e1kCsEnter(E1KSTATE *pState, PPDMCRITSECT pCs, int iBusyRc, RT_SRC_POS_DECL)
{
    int rc = PDMCritSectEnter(pCs, iBusyRc);
    if (RT_FAILURE(rc))
    {
        E1kLog2(("%s ==> FAILED to enter critical section at %s:%d:%s with rc=%Rrc\n",
                INSTANCE(pState), RT_SRC_POS_ARGS, rc));
        PDMDeviceDBGFStop(pState->CTX_SUFF(pDevIns), RT_SRC_POS_ARGS,
                          "%s Failed to enter critical section, rc=%Rrc\n",
                          INSTANCE(pState), rc);
    }
    else
    {
        //E1kLog2(("%s ==> Entered critical section at %s:%d:%s\n", INSTANCE(pState), RT_SRC_POS_ARGS));
    }
    return RT_SUCCESS(rc);
}

DECLINLINE(void) e1kCsLeave(E1KSTATE *pState, PPDMCRITSECT pCs)
{
    //E1kLog2(("%s <== Leaving critical section\n", INSTANCE(pState)));
    PDMCritSectLeave(&pState->cs);
}
# endif
DECLINLINE(int) e1kMutexAcquire(E1KSTATE *pState, int iBusyRc, RT_SRC_POS_DECL)
{
    return VINF_SUCCESS;
}

DECLINLINE(void) e1kMutexRelease(E1KSTATE *pState)
{
}

#endif /* !E1K_GLOBAL_MUTEX */
#ifdef IN_RING3

/**
 * Wakeup the RX thread.
 */
static void e1kWakeupReceive(PPDMDEVINS pDevIns)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    if (    pState->fMaybeOutOfSpace
        &&  pState->hEventMoreRxDescAvail != NIL_RTSEMEVENT)
    {
        STAM_COUNTER_INC(&pState->StatRxOverflowWakeup);
        E1kLog(("%s Waking up Out-of-RX-space semaphore\n",  INSTANCE(pState)));
        RTSemEventSignal(pState->hEventMoreRxDescAvail);
    }
}

/**
 * Hardware reset. Revert all registers to initial values.
 *
 * @param   pState      The device state structure.
 */
static void e1kHardReset(E1KSTATE *pState)
{
    E1kLog(("%s Hard reset triggered\n", INSTANCE(pState)));
    memset(pState->auRegs,        0, sizeof(pState->auRegs));
    memset(pState->aRecAddr.au32, 0, sizeof(pState->aRecAddr.au32));
#ifdef E1K_INIT_RA0
    memcpy(pState->aRecAddr.au32, pState->macConfigured.au8,
           sizeof(pState->macConfigured.au8));
    pState->aRecAddr.array[0].ctl |= RA_CTL_AV;
#endif /* E1K_INIT_RA0 */
    STATUS = 0x0081;    /* SPEED=10b (1000 Mb/s), FD=1b (Full Duplex) */
    EECD   = 0x0100;    /* EE_PRES=1b (EEPROM present) */
    CTRL   = 0x0a09;    /* FRCSPD=1b SPEED=10b LRST=1b FD=1b */
    TSPMT  = 0x01000400;/* TSMT=0400h TSPBP=0100h */
    Assert(GET_BITS(RCTL, BSIZE) == 0);
    pState->u16RxBSize = 2048;

    /* Reset promiscuous mode */
    if (pState->pDrvR3)
        pState->pDrvR3->pfnSetPromiscuousMode(pState->pDrvR3, false);
}

#endif /* IN_RING3 */

/**
 * Compute Internet checksum.
 *
 * @remarks Refer to http://www.netfor2.com/checksum.html for short intro.
 *
 * @param   pState      The device state structure.
 * @param   cpPacket    The packet.
 * @param   cb          The size of the packet.
 * @param   cszText     A string denoting direction of packet transfer.
 *
 * @return  The 1's complement of the 1's complement sum.
 *
 * @thread  E1000_TX
 */
static uint16_t e1kCSum16(const void *pvBuf, size_t cb)
{
    uint32_t  csum = 0;
    uint16_t *pu16 = (uint16_t *)pvBuf;

    while (cb > 1)
    {
        csum += *pu16++;
        cb -= 2;
    }
    if (cb)
        csum += *(uint8_t*)pu16;
    while (csum >> 16)
        csum = (csum >> 16) + (csum & 0xFFFF);
    return ~csum;
}

/**
 * Dump a packet to debug log.
 *
 * @param   pState      The device state structure.
 * @param   cpPacket    The packet.
 * @param   cb          The size of the packet.
 * @param   cszText     A string denoting direction of packet transfer.
 * @thread  E1000_TX
 */
DECLINLINE(void) e1kPacketDump(E1KSTATE* pState, const uint8_t *cpPacket, size_t cb, const char *cszText)
{
#ifdef DEBUG
    if (RT_LIKELY(e1kCsEnter(pState, VERR_SEM_BUSY) == VINF_SUCCESS))
    {
        E1kLog(("%s --- %s packet #%d: ---\n",
                INSTANCE(pState), cszText, ++pState->u32PktNo));
        E1kLog3(("%.*Rhxd\n", cb, cpPacket));
        e1kCsLeave(pState);
    }
#else
    if (RT_LIKELY(e1kCsEnter(pState, VERR_SEM_BUSY) == VINF_SUCCESS))
    {
        E1kLogRel(("E1000: %s packet #%d, seq=%x ack=%x\n", cszText, pState->u32PktNo++, ntohl(*(uint32_t*)(cpPacket+0x26)), ntohl(*(uint32_t*)(cpPacket+0x2A))));
        e1kCsLeave(pState);
    }
#endif
}

/**
 * Determine the type of transmit descriptor.
 *
 * @returns Descriptor type. See E1K_DTYP_XXX defines.
 *
 * @param   pDesc       Pointer to descriptor union.
 * @thread  E1000_TX
 */
DECLINLINE(int) e1kGetDescType(E1KTXDESC* pDesc)
{
    if (pDesc->legacy.cmd.fDEXT)
        return pDesc->context.dw2.u4DTYP;
    return E1K_DTYP_LEGACY;
}

/**
 * Dump receive descriptor to debug log.
 *
 * @param   pState      The device state structure.
 * @param   pDesc       Pointer to the descriptor.
 * @thread  E1000_RX
 */
static void e1kPrintRDesc(E1KSTATE* pState, E1KRXDESC* pDesc)
{
    E1kLog2(("%s <-- Receive Descriptor (%d bytes):\n", INSTANCE(pState), pDesc->u16Length));
    E1kLog2(("        Address=%16LX Length=%04X Csum=%04X\n",
             pDesc->u64BufAddr, pDesc->u16Length, pDesc->u16Checksum));
    E1kLog2(("        STA: %s %s %s %s %s %s %s ERR: %s %s %s %s SPECIAL: %s VLAN=%03x PRI=%x\n",
             pDesc->status.fPIF ? "PIF" : "pif",
             pDesc->status.fIPCS ? "IPCS" : "ipcs",
             pDesc->status.fTCPCS ? "TCPCS" : "tcpcs",
             pDesc->status.fVP ? "VP" : "vp",
             pDesc->status.fIXSM ? "IXSM" : "ixsm",
             pDesc->status.fEOP ? "EOP" : "eop",
             pDesc->status.fDD ? "DD" : "dd",
             pDesc->status.fRXE ? "RXE" : "rxe",
             pDesc->status.fIPE ? "IPE" : "ipe",
             pDesc->status.fTCPE ? "TCPE" : "tcpe",
             pDesc->status.fCE ? "CE" : "ce",
             pDesc->status.fCFI ? "CFI" :"cfi",
             pDesc->status.u12VLAN,
             pDesc->status.u3PRI));
}

/**
 * Dump transmit descriptor to debug log.
 *
 * @param   pState      The device state structure.
 * @param   pDesc       Pointer to descriptor union.
 * @param   cszDir      A string denoting direction of descriptor transfer
 * @thread  E1000_TX
 */
static void e1kPrintTDesc(E1KSTATE* pState, E1KTXDESC* pDesc, const char* cszDir)
{
    switch (e1kGetDescType(pDesc))
    {
        case E1K_DTYP_CONTEXT:
            E1kLog2(("%s %s Context Transmit Descriptor %s\n",
                    INSTANCE(pState), cszDir, cszDir));
            E1kLog2(("        IPCSS=%02X IPCSO=%02X IPCSE=%04X TUCSS=%02X TUCSO=%02X TUCSE=%04X\n",
                    pDesc->context.ip.u8CSS, pDesc->context.ip.u8CSO, pDesc->context.ip.u16CSE,
                    pDesc->context.tu.u8CSS, pDesc->context.tu.u8CSO, pDesc->context.tu.u16CSE));
            E1kLog2(("        TUCMD:%s%s%s %s %s PAYLEN=%04x HDRLEN=%04x MSS=%04x STA: %s\n",
                    pDesc->context.dw2.fIDE ? " IDE":"",
                    pDesc->context.dw2.fRS  ? " RS" :"",
                    pDesc->context.dw2.fTSE ? " TSE":"",
                    pDesc->context.dw2.fIP  ? "IPv4":"IPv6",
                    pDesc->context.dw2.fTCP ?  "TCP":"UDP",
                    pDesc->context.dw2.u20PAYLEN,
                    pDesc->context.dw3.u8HDRLEN,
                    pDesc->context.dw3.u16MSS,
                    pDesc->context.dw3.fDD?"DD":""));
            break;
        case E1K_DTYP_DATA:
            E1kLog2(("%s %s Data Transmit Descriptor (%d bytes) %s\n",
                    INSTANCE(pState), cszDir, pDesc->data.cmd.u20DTALEN, cszDir));
            E1kLog2(("        Address=%16LX DTALEN=%05X\n",
                    pDesc->data.u64BufAddr,
                    pDesc->data.cmd.u20DTALEN));
            E1kLog2(("        DCMD:%s%s%s%s%s%s STA:%s%s%s POPTS:%s%s SPECIAL:%s VLAN=%03x PRI=%x\n",
                    pDesc->data.cmd.fIDE ? " IDE" :"",
                    pDesc->data.cmd.fVLE ? " VLE" :"",
                    pDesc->data.cmd.fRS  ? " RS"  :"",
                    pDesc->data.cmd.fTSE ? " TSE" :"",
                    pDesc->data.cmd.fIFCS? " IFCS":"",
                    pDesc->data.cmd.fEOP ? " EOP" :"",
                    pDesc->data.dw3.fDD  ? " DD"  :"",
                    pDesc->data.dw3.fEC  ? " EC"  :"",
                    pDesc->data.dw3.fLC  ? " LC"  :"",
                    pDesc->data.dw3.fTXSM? " TXSM":"",
                    pDesc->data.dw3.fIXSM? " IXSM":"",
                    pDesc->data.dw3.fCFI ? " CFI" :"",
                    pDesc->data.dw3.u12VLAN,
                    pDesc->data.dw3.u3PRI));
            break;
        case E1K_DTYP_LEGACY:
            E1kLog2(("%s %s Legacy Transmit Descriptor (%d bytes) %s\n",
                    INSTANCE(pState), cszDir, pDesc->legacy.cmd.u16Length, cszDir));
            E1kLog2(("        Address=%16LX DTALEN=%05X\n",
                    pDesc->data.u64BufAddr,
                    pDesc->legacy.cmd.u16Length));
            E1kLog2(("        CMD:%s%s%s%s%s%s STA:%s%s%s CSO=%02x CSS=%02x SPECIAL:%s VLAN=%03x PRI=%x\n",
                    pDesc->legacy.cmd.fIDE ? " IDE" :"",
                    pDesc->legacy.cmd.fVLE ? " VLE" :"",
                    pDesc->legacy.cmd.fRS  ? " RS"  :"",
                    pDesc->legacy.cmd.fIC  ? " IC"  :"",
                    pDesc->legacy.cmd.fIFCS? " IFCS":"",
                    pDesc->legacy.cmd.fEOP ? " EOP" :"",
                    pDesc->legacy.dw3.fDD  ? " DD"  :"",
                    pDesc->legacy.dw3.fEC  ? " EC"  :"",
                    pDesc->legacy.dw3.fLC  ? " LC"  :"",
                    pDesc->legacy.cmd.u8CSO,
                    pDesc->legacy.dw3.u8CSS,
                    pDesc->legacy.dw3.fCFI ? " CFI" :"",
                    pDesc->legacy.dw3.u12VLAN,
                    pDesc->legacy.dw3.u3PRI));
            break;
        default:
            E1kLog(("%s %s Invalid Transmit Descriptor %s\n",
                    INSTANCE(pState), cszDir, cszDir));
            break;
    }
}

/**
 * Raise interrupt if not masked.
 *
 * @param   pState      The device state structure.
 */
static int e1kRaiseInterrupt(E1KSTATE *pState, int rcBusy, uint32_t u32IntCause = 0)
{
    int rc = e1kCsEnter(pState, rcBusy);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;

    E1K_INC_ISTAT_CNT(pState->uStatIntTry);
    ICR |= u32IntCause;
    if (ICR & IMS)
    {
#if 0
        if (pState->fDelayInts)
        {
            E1K_INC_ISTAT_CNT(pState->uStatIntDly);
            pState->iStatIntLostOne = 1;
            E1kLog2(("%s e1kRaiseInterrupt: Delayed. ICR=%08x\n",
                    INSTANCE(pState), ICR));
#define E1K_LOST_IRQ_THRSLD 20
//#define E1K_LOST_IRQ_THRSLD 200000000
            if (pState->iStatIntLost >= E1K_LOST_IRQ_THRSLD)
            {
                E1kLog2(("%s WARNING! Disabling delayed interrupt logic: delayed=%d, delivered=%d\n",
                        INSTANCE(pState), pState->uStatIntDly, pState->uStatIntLate));
                pState->fIntMaskUsed = false;
                pState->uStatDisDly++;
            }
        }
        else
#endif
        if (pState->fIntRaised)
        {
            E1K_INC_ISTAT_CNT(pState->uStatIntSkip);
            E1kLog2(("%s e1kRaiseInterrupt: Already raised, skipped. ICR&IMS=%08x\n",
                    INSTANCE(pState), ICR & IMS));
        }
        else
        {
#ifdef E1K_ITR_ENABLED
            uint64_t tstamp = TMTimerGet(pState->CTX_SUFF(pIntTimer));
            /* interrupts/sec = 1 / (256 * 10E-9 * ITR) */
            E1kLog2(("%s e1kRaiseInterrupt: tstamp - pState->u64AckedAt = %d, ITR * 256 = %d\n",
                        INSTANCE(pState), (uint32_t)(tstamp - pState->u64AckedAt), ITR * 256));
            if (!!ITR && pState->fIntMaskUsed && tstamp - pState->u64AckedAt < ITR * 256)
            {
                E1K_INC_ISTAT_CNT(pState->uStatIntEarly);
                E1kLog2(("%s e1kRaiseInterrupt: Too early to raise again: %d ns < %d ns.\n",
                        INSTANCE(pState), (uint32_t)(tstamp - pState->u64AckedAt), ITR * 256));
            }
            else
#endif
            {

                /* Since we are delivering the interrupt now
                 * there is no need to do it later -- stop the timer.
                 */
                TMTimerStop(pState->CTX_SUFF(pIntTimer));
                E1K_INC_ISTAT_CNT(pState->uStatInt);
                STAM_COUNTER_INC(&pState->StatIntsRaised);
                /* Got at least one unmasked interrupt cause */
                pState->fIntRaised = true;
                /* Raise(1) INTA(0) */
                //e1kMutexRelease(pState);
                E1kLogRel(("E1000: irq RAISED icr&mask=0x%x, icr=0x%x\n", ICR & IMS, ICR));
                PDMDevHlpPCISetIrq(pState->CTX_SUFF(pDevIns), 0, 1);
                //e1kMutexAcquire(pState, RT_SRC_POS);
                E1kLog(("%s e1kRaiseInterrupt: Raised. ICR&IMS=%08x\n",
                        INSTANCE(pState), ICR & IMS));
            }
        }
    }
    else
    {
        E1K_INC_ISTAT_CNT(pState->uStatIntMasked);
        E1kLog2(("%s e1kRaiseInterrupt: Not raising, ICR=%08x, IMS=%08x\n",
                INSTANCE(pState), ICR, IMS));
    }
    e1kCsLeave(pState);
    return VINF_SUCCESS;
}

/**
 * Compute the physical address of the descriptor.
 *
 * @returns the physical address of the descriptor.
 *
 * @param   baseHigh        High-order 32 bits of descriptor table address.
 * @param   baseLow         Low-order 32 bits of descriptor table address.
 * @param   idxDesc         The descriptor index in the table.
 */
DECLINLINE(RTGCPHYS) e1kDescAddr(uint32_t baseHigh, uint32_t baseLow, uint32_t idxDesc)
{
    AssertCompile(sizeof(E1KRXDESC) == sizeof(E1KTXDESC));
    return ((uint64_t)baseHigh << 32) + baseLow + idxDesc * sizeof(E1KRXDESC);
}

/**
 * Advance the head pointer of the receive descriptor queue.
 *
 * @remarks RDH always points to the next available RX descriptor.
 *
 * @param   pState      The device state structure.
 */
DECLINLINE(void) e1kAdvanceRDH(E1KSTATE *pState)
{
    //e1kCsEnter(pState, RT_SRC_POS);
    if (++RDH * sizeof(E1KRXDESC) >= RDLEN)
        RDH = 0;
    /*
     * Compute current receive queue length and fire RXDMT0 interrupt
     * if we are low on receive buffers
     */
    uint32_t uRQueueLen = RDH>RDT ? RDLEN/sizeof(E1KRXDESC)-RDH+RDT : RDT-RDH;
    /*
     * The minimum threshold is controlled by RDMTS bits of RCTL:
     * 00 = 1/2 of RDLEN
     * 01 = 1/4 of RDLEN
     * 10 = 1/8 of RDLEN
     * 11 = reserved
     */
    uint32_t uMinRQThreshold = RDLEN / sizeof(E1KRXDESC) / (2 << GET_BITS(RCTL, RDMTS));
    if (uRQueueLen <= uMinRQThreshold)
    {
        E1kLogRel(("E1000: low on RX descriptors, RDH=%x RDT=%x len=%x threshold=%x\n", RDH, RDT, uRQueueLen, uMinRQThreshold));
        E1kLog2(("%s Low on RX descriptors, RDH=%x RDT=%x len=%x threshold=%x, raise an interrupt\n",
                 INSTANCE(pState), RDH, RDT, uRQueueLen, uMinRQThreshold));
        E1K_INC_ISTAT_CNT(pState->uStatIntRXDMT0);
        e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_RXDMT0);
    }
    E1kLog2(("%s e1kAdvanceRDH: at exit RDH=%x RDT=%x len=%x\n",
             INSTANCE(pState), RDH, RDT, uRQueueLen));
    //e1kCsLeave(pState);
}

/**
 * Store a fragment of received packet that fits into the next available RX
 * buffer.
 *
 * @remarks Trigger the RXT0 interrupt if it is the last fragment of the packet.
 *
 * @param   pState          The device state structure.
 * @param   pDesc           The next available RX descriptor.
 * @param   pvBuf           The fragment.
 * @param   cb              The size of the fragment.
 */
static DECLCALLBACK(void) e1kStoreRxFragment(E1KSTATE *pState, E1KRXDESC *pDesc, const void *pvBuf, size_t cb)
{
    STAM_PROFILE_ADV_START(&pState->StatReceiveStore, a);
    E1kLog2(("%s e1kStoreRxFragment: store fragment of %04X at %016LX, EOP=%d\n", pState->szInstance, cb, pDesc->u64BufAddr, pDesc->status.fEOP));
    PDMDevHlpPhysWrite(pState->CTX_SUFF(pDevIns), pDesc->u64BufAddr, pvBuf, cb);
    pDesc->u16Length = (uint16_t)cb;                        Assert(pDesc->u16Length == cb);
    /* Write back the descriptor */
    PDMDevHlpPhysWrite(pState->CTX_SUFF(pDevIns), e1kDescAddr(RDBAH, RDBAL, RDH), pDesc, sizeof(E1KRXDESC));
    e1kPrintRDesc(pState, pDesc);
    E1kLogRel(("E1000: Wrote back RX desc, RDH=%x\n", RDH));
    /* Advance head */
    e1kAdvanceRDH(pState);
    //E1kLog2(("%s e1kStoreRxFragment: EOP=%d RDTR=%08X RADV=%08X\n", INSTANCE(pState), pDesc->fEOP, RDTR, RADV));
    if (pDesc->status.fEOP)
    {
        /* Complete packet has been stored -- it is time to let the guest know. */
#ifdef E1K_USE_RX_TIMERS
        if (RDTR)
        {
            /* Arm the timer to fire in RDTR usec (discard .024) */
            e1kArmTimer(pState, pState->CTX_SUFF(pRIDTimer), RDTR);
            /* If absolute timer delay is enabled and the timer is not running yet, arm it. */
            if (RADV != 0 && !TMTimerIsActive(pState->CTX_SUFF(pRADTimer)))
                e1kArmTimer(pState, pState->CTX_SUFF(pRADTimer), RADV);
        }
        else
        {
#endif
            /* 0 delay means immediate interrupt */
            E1K_INC_ISTAT_CNT(pState->uStatIntRx);
            e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_RXT0);
#ifdef E1K_USE_RX_TIMERS
        }
#endif
    }
    STAM_PROFILE_ADV_STOP(&pState->StatReceiveStore, a);
}

/**
 * Returns true if it is a broadcast packet.
 *
 * @returns true if destination address indicates broadcast.
 * @param   pvBuf           The ethernet packet.
 */
DECLINLINE(bool) e1kIsBroadcast(const void *pvBuf)
{
    static const uint8_t s_abBcastAddr[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    return memcmp(pvBuf, s_abBcastAddr, sizeof(s_abBcastAddr)) == 0;
}

/**
 * Returns true if it is a multicast packet.
 *
 * @remarks returns true for broadcast packets as well.
 * @returns true if destination address indicates multicast.
 * @param   pvBuf           The ethernet packet.
 */
DECLINLINE(bool) e1kIsMulticast(const void *pvBuf)
{
    return (*(char*)pvBuf) & 1;
}

/**
 * Set IXSM, IPCS and TCPCS flags according to the packet type.
 *
 * @remarks We emulate checksum offloading for major packets types only.
 *
 * @returns VBox status code.
 * @param   pState          The device state structure.
 * @param   pFrame          The available data.
 * @param   cb              Number of bytes available in the buffer.
 * @param   status          Bit fields containing status info.
 */
static int e1kRxChecksumOffload(E1KSTATE* pState, const uint8_t *pFrame, size_t cb, E1KRXDST *pStatus)
{
    /** @todo
     * It is not safe to bypass checksum verification for packets coming
     * from real wire. We currently unable to tell where packets are
     * coming from so we tell the driver to ignore our checksum flags
     * and do verification in software.
     */
#if 0
    uint16_t uEtherType = ntohs(*(uint16_t*)(pFrame + 12));

    E1kLog2(("%s e1kRxChecksumOffload: EtherType=%x\n", INSTANCE(pState), uEtherType));

    switch (uEtherType)
    {
        case 0x800: /* IPv4 */
        {
            pStatus->fIXSM  = false;
            pStatus->fIPCS  = true;
            PRTNETIPV4 pIpHdr4 = (PRTNETIPV4)(pFrame + 14);
            /* TCP/UDP checksum offloading works with TCP and UDP only */
            pStatus->fTCPCS = pIpHdr4->ip_p == 6 || pIpHdr4->ip_p == 17;
            break;
        }
        case 0x86DD: /* IPv6 */
            pStatus->fIXSM = false;
            pStatus->fIPCS  = false;
            pStatus->fTCPCS = true;
            break;
        default: /* ARP, VLAN, etc. */
            pStatus->fIXSM = true;
            break;
    }
#else
    pStatus->fIXSM = true;
#endif
    return VINF_SUCCESS;
}

/**
 * Pad and store received packet.
 *
 * @remarks Make sure that the packet appears to upper layer as one coming
 *          from real Ethernet: pad it and insert FCS.
 *
 * @returns VBox status code.
 * @param   pState          The device state structure.
 * @param   pvBuf           The available data.
 * @param   cb              Number of bytes available in the buffer.
 * @param   status          Bit fields containing status info.
 */
static int e1kHandleRxPacket(E1KSTATE* pState, const void *pvBuf, size_t cb, E1KRXDST status)
{
#if defined(IN_RING3) /** @todo Remove this extra copying, it's gonna make us run out of kernel / hypervisor stack! */
    uint8_t   rxPacket[E1K_MAX_RX_PKT_SIZE];
    uint8_t  *ptr = rxPacket;

#ifndef E1K_GLOBAL_MUTEX
    int rc = e1kCsRxEnter(pState, VERR_SEM_BUSY);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
#endif

    if (cb > 70) /* unqualified guess */
        pState->led.Asserted.s.fReading = pState->led.Actual.s.fReading = 1;

    Assert(cb <= E1K_MAX_RX_PKT_SIZE);
    memcpy(rxPacket, pvBuf, cb);

    size_t cbMax = ((RCTL & RCTL_LPE) ? E1K_MAX_RX_PKT_SIZE - 4 : 1518) - (status.fVP ? 0 : 4);
    E1kLog3(("%s Max RX packet size is %u\n", INSTANCE(pState), cbMax));

    /* Pad short packets */
    if (cb < 60)
    {
        memset(rxPacket + cb, 0, 60 - cb);
        cb = 60;
    }
    if (!(RCTL & RCTL_SECRC) && cb <= cbMax)
    {
        /* Add FCS if CRC stripping is not enabled */
        *(uint32_t*)(rxPacket + cb) = RTCrc32(rxPacket, cb);
        cb += sizeof(uint32_t);
        E1kLog3(("%s Added FCS (cb=%u)\n", INSTANCE(pState), cb));
    }
    /* Compute checksum of complete packet */
    uint16_t checksum = e1kCSum16(rxPacket + GET_BITS(RXCSUM, PCSS), cb);
    e1kRxChecksumOffload(pState, rxPacket, cb, &status);

    /* Update stats */
    E1K_INC_CNT32(GPRC);
    if (e1kIsBroadcast(pvBuf))
        E1K_INC_CNT32(BPRC);
    else if (e1kIsMulticast(pvBuf))
        E1K_INC_CNT32(MPRC);
    /* Update octet receive counter */
    E1K_ADD_CNT64(GORCL, GORCH, cb);
    STAM_REL_COUNTER_ADD(&pState->StatReceiveBytes, cb);
    if (cb == 64)
        E1K_INC_CNT32(PRC64);
    else if (cb < 128)
        E1K_INC_CNT32(PRC127);
    else if (cb < 256)
        E1K_INC_CNT32(PRC255);
    else if (cb < 512)
        E1K_INC_CNT32(PRC511);
    else if (cb < 1024)
        E1K_INC_CNT32(PRC1023);
    else
        E1K_INC_CNT32(PRC1522);

    E1K_INC_ISTAT_CNT(pState->uStatRxFrm);

    if (RDH == RDT)
    {
        E1kLog(("%s Out of receive buffers, dropping the packet",
                INSTANCE(pState)));
    }
    /* Store the packet to receive buffers */
    while (RDH != RDT)
    {
        /* Load the descriptor pointed by head */
        E1KRXDESC desc;
        PDMDevHlpPhysRead(pState->CTX_SUFF(pDevIns), e1kDescAddr(RDBAH, RDBAL, RDH),
                          &desc, sizeof(desc));
        if (desc.u64BufAddr)
        {
            /* Update descriptor */
            desc.status        = status;
            desc.u16Checksum   = checksum;
            desc.status.fDD    = true;

            /*
             * We need to leave Rx critical section here or we risk deadlocking
             * with EMT in e1kRegWriteRDT when the write is to an unallocated
             * page or has an access handler associated with it.
             * Note that it is safe to leave the critical section here since e1kRegWriteRDT()
             * modifies RDT only.
             */
            if (cb > pState->u16RxBSize)
            {
                desc.status.fEOP = false;
                e1kCsRxLeave(pState);
                e1kStoreRxFragment(pState, &desc, ptr, pState->u16RxBSize);
                rc = e1kCsRxEnter(pState, VERR_SEM_BUSY);
                if (RT_UNLIKELY(rc != VINF_SUCCESS))
                    return rc;
                ptr += pState->u16RxBSize;
                cb -= pState->u16RxBSize;
            }
            else
            {
                desc.status.fEOP = true;
                e1kCsRxLeave(pState);
                e1kStoreRxFragment(pState, &desc, ptr, cb);
                pState->led.Actual.s.fReading = 0;
                return VINF_SUCCESS;
            }
            /* Note: RDH is advanced by e1kStoreRxFragment! */
        }
        else
        {
            desc.status.fDD = true;
            PDMDevHlpPhysWrite(pState->CTX_SUFF(pDevIns),
                               e1kDescAddr(RDBAH, RDBAL, RDH),
                                           &desc, sizeof(desc));
            e1kAdvanceRDH(pState);
        }
    }

    if (cb > 0)
        E1kLog(("%s Out of receive buffers, dropping %u bytes", INSTANCE(pState), cb));

    pState->led.Actual.s.fReading = 0;

    e1kCsRxLeave(pState);

    return VINF_SUCCESS;
#else
    return VERR_INTERNAL_ERROR_2;
#endif
}


#if 0 /* unused */
/**
 * Read handler for Device Status register.
 *
 * Get the link status from PHY.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   mask        Used to implement partial reads (8 and 16-bit).
 */
static int e1kRegReadCTRL(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    E1kLog(("%s e1kRegReadCTRL: mdio dir=%s mdc dir=%s mdc=%d\n",
            INSTANCE(pState), (CTRL & CTRL_MDIO_DIR)?"OUT":"IN ",
            (CTRL & CTRL_MDC_DIR)?"OUT":"IN ", !!(CTRL & CTRL_MDC)));
    if ((CTRL & CTRL_MDIO_DIR) == 0 && (CTRL & CTRL_MDC))
    {
        /* MDC is high and MDIO pin is used for input, read MDIO pin from PHY */
        if (Phy::readMDIO(&pState->phy))
            *pu32Value = CTRL | CTRL_MDIO;
        else
            *pu32Value = CTRL & ~CTRL_MDIO;
        E1kLog(("%s e1kRegReadCTRL: Phy::readMDIO(%d)\n",
                INSTANCE(pState), !!(*pu32Value & CTRL_MDIO)));
    }
    else
    {
        /* MDIO pin is used for output, ignore it */
        *pu32Value = CTRL;
    }
    return VINF_SUCCESS;
}
#endif /* unused */

/**
 * Write handler for Device Control register.
 *
 * Handles reset.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteCTRL(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    int rc = VINF_SUCCESS;

    if (value & CTRL_RESET)
    { /* RST */
#ifndef IN_RING3
        return VINF_IOM_HC_IOPORT_WRITE;
#else
        e1kHardReset(pState);
#endif
    }
    else
    {
        if (   (value & CTRL_SLU)
            && pState->fCableConnected
            && !(STATUS & STATUS_LU))
        {
            /* The driver indicates that we should bring up the link */
            /* Do so in 5 seconds. */
            e1kArmTimer(pState, pState->CTX_SUFF(pLUTimer), 5000000);
            /*
             * Change the status (but not PHY status) anyway as Windows expects
             * it for 82543GC.
             */
            STATUS |= STATUS_LU;
        }
        if (value & CTRL_VME)
        {
            E1kLog(("%s VLAN Mode is not supported yet!\n", INSTANCE(pState)));
        }
        E1kLog(("%s e1kRegWriteCTRL: mdio dir=%s mdc dir=%s mdc=%s mdio=%d\n",
                INSTANCE(pState), (value & CTRL_MDIO_DIR)?"OUT":"IN ",
                (value & CTRL_MDC_DIR)?"OUT":"IN ", (value & CTRL_MDC)?"HIGH":"LOW ", !!(value & CTRL_MDIO)));
        if (value & CTRL_MDC)
        {
            if (value & CTRL_MDIO_DIR)
            {
                E1kLog(("%s e1kRegWriteCTRL: Phy::writeMDIO(%d)\n", INSTANCE(pState), !!(value & CTRL_MDIO)));
                /* MDIO direction pin is set to output and MDC is high, write MDIO pin value to PHY */
                Phy::writeMDIO(&pState->phy, !!(value & CTRL_MDIO));
            }
            else
            {
                if (Phy::readMDIO(&pState->phy))
                    value |= CTRL_MDIO;
                else
                    value &= ~CTRL_MDIO;
                E1kLog(("%s e1kRegWriteCTRL: Phy::readMDIO(%d)\n",
                        INSTANCE(pState), !!(value & CTRL_MDIO)));
            }
        }
        rc = e1kRegWriteDefault(pState, offset, index, value);
    }

    return rc;
}

/**
 * Write handler for EEPROM/Flash Control/Data register.
 *
 * Handles EEPROM access requests; forwards writes to EEPROM device if access has been granted.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteEECD(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
#ifdef IN_RING3
    /* So far we are concerned with lower byte only */
    if ((EECD & EECD_EE_GNT) || pState->eChip == E1K_CHIP_82543GC)
    {
        /* Access to EEPROM granted -- forward 4-wire bits to EEPROM device */
        /* Note: 82543GC does not need to request EEPROM access */
        STAM_PROFILE_ADV_START(&pState->StatEEPROMWrite, a);
        pState->eeprom.write(value & EECD_EE_WIRES);
        STAM_PROFILE_ADV_STOP(&pState->StatEEPROMWrite, a);
    }
    if (value & EECD_EE_REQ)
        EECD |= EECD_EE_REQ|EECD_EE_GNT;
    else
        EECD &= ~EECD_EE_GNT;
    //e1kRegWriteDefault(pState, offset, index, value );

    return VINF_SUCCESS;
#else /* !IN_RING3 */
    return VINF_IOM_HC_MMIO_WRITE;
#endif /* !IN_RING3 */
}

/**
 * Read handler for EEPROM/Flash Control/Data register.
 *
 * Lower 4 bits come from EEPROM device if EEPROM access has been granted.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   mask        Used to implement partial reads (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegReadEECD(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
#ifdef IN_RING3
    uint32_t value;
    int      rc = e1kRegReadDefault(pState, offset, index, &value);
    if (RT_SUCCESS(rc))
    {
        if ((value & EECD_EE_GNT) || pState->eChip == E1K_CHIP_82543GC)
        {
            /* Note: 82543GC does not need to request EEPROM access */
            /* Access to EEPROM granted -- get 4-wire bits to EEPROM device */
            STAM_PROFILE_ADV_START(&pState->StatEEPROMRead, a);
            value |= pState->eeprom.read();
            STAM_PROFILE_ADV_STOP(&pState->StatEEPROMRead, a);
        }
        *pu32Value = value;
    }

    return rc;
#else /* !IN_RING3 */
    return VINF_IOM_HC_MMIO_READ;
#endif /* !IN_RING3 */
}

/**
 * Write handler for EEPROM Read register.
 *
 * Handles EEPROM word access requests, reads EEPROM and stores the result
 * into DATA field.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteEERD(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
#ifdef IN_RING3
    /* Make use of 'writable' and 'readable' masks. */
    e1kRegWriteDefault(pState, offset, index, value);
    /* DONE and DATA are set only if read was triggered by START. */
    if (value & EERD_START)
    {
        uint16_t tmp;
        STAM_PROFILE_ADV_START(&pState->StatEEPROMRead, a);
        if (pState->eeprom.readWord(GET_BITS_V(value, EERD, ADDR), &tmp))
            SET_BITS(EERD, DATA, tmp);
        EERD |= EERD_DONE;
        STAM_PROFILE_ADV_STOP(&pState->StatEEPROMRead, a);
    }

    return VINF_SUCCESS;
#else /* !IN_RING3 */
    return VINF_IOM_HC_MMIO_WRITE;
#endif /* !IN_RING3 */
}


/**
 * Write handler for MDI Control register.
 *
 * Handles PHY read/write requests; forwards requests to internal PHY device.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteMDIC(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    if (value & MDIC_INT_EN)
    {
        E1kLog(("%s ERROR! Interrupt at the end of an MDI cycle is not supported yet.\n",
                INSTANCE(pState)));
    }
    else if (value & MDIC_READY)
    {
        E1kLog(("%s ERROR! Ready bit is not reset by software during write operation.\n",
                INSTANCE(pState)));
    }
    else if (GET_BITS_V(value, MDIC, PHY) != 1)
    {
        E1kLog(("%s ERROR! Access to invalid PHY detected, phy=%d.\n",
                INSTANCE(pState), GET_BITS_V(value, MDIC, PHY)));
    }
    else
    {
        /* Store the value */
        e1kRegWriteDefault(pState, offset, index, value);
        STAM_COUNTER_INC(&pState->StatPHYAccesses);
        /* Forward op to PHY */
        if (value & MDIC_OP_READ)
            SET_BITS(MDIC, DATA, Phy::readRegister(&pState->phy, GET_BITS_V(value, MDIC, REG)));
        else
            Phy::writeRegister(&pState->phy, GET_BITS_V(value, MDIC, REG), value & MDIC_DATA_MASK);
        /* Let software know that we are done */
        MDIC |= MDIC_READY;
    }

    return VINF_SUCCESS;
}

/**
 * Write handler for Interrupt Cause Read register.
 *
 * Bits corresponding to 1s in 'value' will be cleared in ICR register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteICR(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    ICR &= ~value;

    return VINF_SUCCESS;
}

/**
 * Read handler for Interrupt Cause Read register.
 *
 * Reading this register acknowledges all interrupts.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   mask        Not used.
 * @thread  EMT
 */
static int e1kRegReadICR(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    int rc = e1kCsEnter(pState, VINF_IOM_HC_MMIO_READ);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;

    uint32_t value = 0;
    rc = e1kRegReadDefault(pState, offset, index, &value);
    if (RT_SUCCESS(rc))
    {
        if (value)
        {
            /*
             * Not clearing ICR causes QNX to hang as it reads ICR in a loop
             * with disabled interrupts.
             */
            //if (IMS)
            if (1)
            {
                /*
                 * Interrupts were enabled -- we are supposedly at the very
                 * beginning of interrupt handler
                 */
                E1kLogRel(("E1000: irq lowered, icr=0x%x\n", ICR));
                E1kLog(("%s e1kRegReadICR: Lowered IRQ (%08x)\n", INSTANCE(pState), ICR));
                /* Clear all pending interrupts */
                ICR = 0;
                pState->fIntRaised = false;
                /* Lower(0) INTA(0) */
                //e1kMutexRelease(pState);
                PDMDevHlpPCISetIrq(pState->CTX_SUFF(pDevIns), 0, 0);
                //e1kMutexAcquire(pState, RT_SRC_POS);

                pState->u64AckedAt = TMTimerGet(pState->CTX_SUFF(pIntTimer));
                if (pState->fIntMaskUsed)
                    pState->fDelayInts = true;
            }
            else
            {
                /*
                 * Interrupts are disabled -- in windows guests ICR read is done
                 * just before re-enabling interrupts
                 */
                E1kLog(("%s e1kRegReadICR: Suppressing auto-clear due to disabled interrupts (%08x)\n", INSTANCE(pState), ICR));
            }
        }
        *pu32Value = value;
    }
    e1kCsLeave(pState);

    return rc;
}

/**
 * Write handler for Interrupt Cause Set register.
 *
 * Bits corresponding to 1s in 'value' will be set in ICR register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteICS(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    E1K_INC_ISTAT_CNT(pState->uStatIntICS);
    return e1kRaiseInterrupt(pState, VINF_IOM_HC_MMIO_WRITE, value & s_e1kRegMap[ICS_IDX].writable);
}

/**
 * Write handler for Interrupt Mask Set register.
 *
 * Will trigger pending interrupts.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteIMS(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    IMS |= value;
    E1kLogRel(("E1000: irq enabled, RDH=%x RDT=%x TDH=%x TDT=%x\n", RDH, RDT, TDH, TDT));
    E1kLog(("%s e1kRegWriteIMS: IRQ enabled\n", INSTANCE(pState)));
    /* Mask changes, we need to raise pending interrupts. */
    if ((ICR & IMS) && !pState->fLocked)
    {
        E1kLog2(("%s e1kRegWriteIMS: IRQ pending (%08x), arming late int timer...\n",
                 INSTANCE(pState), ICR));
        /* Raising an interrupt immediately causes win7 to hang upon NIC reconfiguration (#5023) */
        TMTimerSet(pState->CTX_SUFF(pIntTimer), TMTimerFromNano(pState->CTX_SUFF(pIntTimer), ITR * 256) +
                   TMTimerGet(pState->CTX_SUFF(pIntTimer)));
    }

    return VINF_SUCCESS;
}

/**
 * Write handler for Interrupt Mask Clear register.
 *
 * Bits corresponding to 1s in 'value' will be cleared in IMS register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteIMC(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    int rc = e1kCsEnter(pState, VINF_IOM_HC_MMIO_WRITE);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    if (pState->fIntRaised)
    {
        /*
         * Technically we should reset fIntRaised in ICR read handler, but it will cause
         * Windows to freeze since it may receive an interrupt while still in the very beginning
         * of interrupt handler.
         */
        E1K_INC_ISTAT_CNT(pState->uStatIntLower);
        STAM_COUNTER_INC(&pState->StatIntsPrevented);
        E1kLogRel(("E1000: irq lowered (IMC), icr=0x%x\n", ICR));
        /* Lower(0) INTA(0) */
        PDMDevHlpPCISetIrq(pState->CTX_SUFF(pDevIns), 0, 0);
        pState->fIntRaised = false;
        E1kLog(("%s e1kRegWriteIMC: Lowered IRQ: ICR=%08x\n", INSTANCE(pState), ICR));
    }
    IMS &= ~value;
    E1kLog(("%s e1kRegWriteIMC: IRQ disabled\n", INSTANCE(pState)));
    e1kCsLeave(pState);

    return VINF_SUCCESS;
}

/**
 * Write handler for Receive Control register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteRCTL(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    /* Update promiscuous mode */
    bool fBecomePromiscous = !!(value & (RCTL_UPE | RCTL_MPE));
    if (fBecomePromiscous != !!( RCTL & (RCTL_UPE | RCTL_MPE)))
    {
        /* Promiscuity has changed, pass the knowledge on. */
#ifndef IN_RING3
        return VINF_IOM_HC_IOPORT_WRITE;
#else
        if (pState->pDrvR3)
            pState->pDrvR3->pfnSetPromiscuousMode(pState->pDrvR3, fBecomePromiscous);
#endif
    }

    /* Adjust receive buffer size */
    unsigned cbRxBuf = 2048 >> GET_BITS_V(value, RCTL, BSIZE);
    if (value & RCTL_BSEX)
        cbRxBuf *= 16;
    if (cbRxBuf != pState->u16RxBSize)
        E1kLog2(("%s e1kRegWriteRCTL: Setting receive buffer size to %d (old %d)\n",
                 INSTANCE(pState), cbRxBuf, pState->u16RxBSize));
    pState->u16RxBSize = cbRxBuf;

    /* Update the register */
    e1kRegWriteDefault(pState, offset, index, value);

    return VINF_SUCCESS;
}

/**
 * Write handler for Packet Buffer Allocation register.
 *
 * TXA = 64 - RXA.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWritePBA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    e1kRegWriteDefault(pState, offset, index, value);
    PBA_st->txa = 64 - PBA_st->rxa;

    return VINF_SUCCESS;
}

/**
 * Write handler for Receive Descriptor Tail register.
 *
 * @remarks Write into RDT forces switch to HC and signal to
 *          e1kNetworkDown_WaitReceiveAvail().
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteRDT(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
#ifndef IN_RING3
    /* XXX */
//    return VINF_IOM_HC_MMIO_WRITE;
#endif
    int rc = e1kCsRxEnter(pState, VINF_IOM_HC_MMIO_WRITE);
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        E1kLog(("%s e1kRegWriteRDT\n",  INSTANCE(pState)));
        rc = e1kRegWriteDefault(pState, offset, index, value);
        e1kCsRxLeave(pState);
        if (RT_SUCCESS(rc))
        {
/** @todo bird: Use SUPSem* for this so we can signal it in ring-0 as well
 *        without requiring any context switches.  We should also check the
 *        wait condition before bothering to queue the item as we're currently
 *        queuing thousands of items per second here in a normal transmit
 *        scenario.  Expect performance changes when fixing this! */
#ifdef IN_RING3
            /* Signal that we have more receive descriptors available. */
            e1kWakeupReceive(pState->CTX_SUFF(pDevIns));
#else
            PPDMQUEUEITEMCORE pItem = PDMQueueAlloc(pState->CTX_SUFF(pCanRxQueue));
            if (pItem)
                PDMQueueInsert(pState->CTX_SUFF(pCanRxQueue), pItem);
#endif
        }
    }
    return rc;
}

/**
 * Write handler for Receive Delay Timer register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteRDTR(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    e1kRegWriteDefault(pState, offset, index, value);
    if (value & RDTR_FPD)
    {
        /* Flush requested, cancel both timers and raise interrupt */
#ifdef E1K_USE_RX_TIMERS
        e1kCancelTimer(pState, pState->CTX_SUFF(pRIDTimer));
        e1kCancelTimer(pState, pState->CTX_SUFF(pRADTimer));
#endif
        E1K_INC_ISTAT_CNT(pState->uStatIntRDTR);
        return e1kRaiseInterrupt(pState, VINF_IOM_HC_MMIO_WRITE, ICR_RXT0);
    }

    return VINF_SUCCESS;
}

DECLINLINE(uint32_t) e1kGetTxLen(E1KSTATE* pState)
{
    /**
     *  Make sure TDT won't change during computation. EMT may modify TDT at
     *  any moment.
     */
    uint32_t tdt = TDT;
    return (TDH>tdt ? TDLEN/sizeof(E1KTXDESC) : 0) + tdt - TDH;
}

#ifdef IN_RING3
#ifdef E1K_USE_TX_TIMERS

/**
 * Transmit Interrupt Delay Timer handler.
 *
 * @remarks We only get here when the timer expires.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kTxIntDelayTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        E1K_INC_ISTAT_CNT(pState->uStatTID);
        /* Cancel absolute delay timer as we have already got attention */
#ifndef E1K_NO_TAD
        e1kCancelTimer(pState, pState->CTX_SUFF(pTADTimer));
#endif /* E1K_NO_TAD */
        e1kRaiseInterrupt(pState, ICR_TXDW);
        e1kMutexRelease(pState);
    }
}

/**
 * Transmit Absolute Delay Timer handler.
 *
 * @remarks We only get here when the timer expires.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kTxAbsDelayTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        E1K_INC_ISTAT_CNT(pState->uStatTAD);
        /* Cancel interrupt delay timer as we have already got attention */
        e1kCancelTimer(pState, pState->CTX_SUFF(pTIDTimer));
        e1kRaiseInterrupt(pState, ICR_TXDW);
        e1kMutexRelease(pState);
    }
}

#endif /* E1K_USE_TX_TIMERS */
#ifdef E1K_USE_RX_TIMERS

/**
 * Receive Interrupt Delay Timer handler.
 *
 * @remarks We only get here when the timer expires.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kRxIntDelayTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        E1K_INC_ISTAT_CNT(pState->uStatRID);
        /* Cancel absolute delay timer as we have already got attention */
        e1kCancelTimer(pState, pState->CTX_SUFF(pRADTimer));
        e1kRaiseInterrupt(pState, ICR_RXT0);
        e1kMutexRelease(pState);
    }
}

/**
 * Receive Absolute Delay Timer handler.
 *
 * @remarks We only get here when the timer expires.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kRxAbsDelayTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        E1K_INC_ISTAT_CNT(pState->uStatRAD);
        /* Cancel interrupt delay timer as we have already got attention */
        e1kCancelTimer(pState, pState->CTX_SUFF(pRIDTimer));
        e1kRaiseInterrupt(pState, ICR_RXT0);
        e1kMutexRelease(pState);
    }
}

#endif /* E1K_USE_RX_TIMERS */

/**
 * Late Interrupt Timer handler.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kLateIntTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    STAM_PROFILE_ADV_START(&pState->StatLateIntTimer, a);
    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        STAM_COUNTER_INC(&pState->StatLateInts);
        E1K_INC_ISTAT_CNT(pState->uStatIntLate);
#if 0
        if (pState->iStatIntLost > -100)
            pState->iStatIntLost--;
#endif
        e1kRaiseInterrupt(pState, VERR_SEM_BUSY, 0);
        e1kMutexRelease(pState);
    }
    STAM_PROFILE_ADV_STOP(&pState->StatLateIntTimer, a);
}

/**
 * Link Up Timer handler.
 *
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pTimer      Pointer to the timer.
 * @param   pvUser      NULL.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kLinkUpTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    E1KSTATE *pState = (E1KSTATE *)pvUser;

    /*
     * This can happen if we set the link status to down when the Link up timer was
     * already armed (shortly after e1kLoadDone() or when the cable was disconnected
     * and connect+disconnect the cable very quick.
     */
    if (!pState->fCableConnected)
        return;

    if (RT_LIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) == VINF_SUCCESS))
    {
        STATUS |= STATUS_LU;
        Phy::setLinkStatus(&pState->phy, true);
        e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_LSC);
        e1kMutexRelease(pState);
    }
}

#endif /* IN_RING3 */

/**
 * Sets up the GSO context according to the TSE new context descriptor.
 *
 * @param   pGso                The GSO context to setup.
 * @param   pCtx                The context descriptor.
 */
DECLINLINE(void) e1kSetupGsoCtx(PPDMNETWORKGSO pGso, E1KTXCTX const *pCtx)
{
    pGso->u8Type = PDMNETWORKGSOTYPE_INVALID;

    /*
     * See if the context descriptor describes something that could be TCP or
     * UDP over IPv[46].
     */
    /* Check the header ordering and spacing: 1. Ethernet, 2. IP, 3. TCP/UDP. */
    if (RT_UNLIKELY( pCtx->ip.u8CSS < sizeof(RTNETETHERHDR) ))
    {
        E1kLog(("e1kSetupGsoCtx: IPCSS=%#x\n", pCtx->ip.u8CSS));
        return;
    }
    if (RT_UNLIKELY( pCtx->tu.u8CSS     < (size_t)pCtx->ip.u8CSS + (pCtx->dw2.fIP  ? RTNETIPV4_MIN_LEN : RTNETIPV6_MIN_LEN) ))
    {
        E1kLog(("e1kSetupGsoCtx: TUCSS=%#x\n", pCtx->tu.u8CSS));
        return;
    }
    if (RT_UNLIKELY(   pCtx->dw2.fTCP
                     ? pCtx->dw3.u8HDRLEN <  (size_t)pCtx->tu.u8CSS + RTNETTCP_MIN_LEN
                     : pCtx->dw3.u8HDRLEN != (size_t)pCtx->tu.u8CSS + RTNETUDP_MIN_LEN ))
    {
        E1kLog(("e1kSetupGsoCtx: HDRLEN=%#x TCP=%d\n", pCtx->dw3.u8HDRLEN, pCtx->dw2.fTCP));
        return;
    }

    /* The end of the TCP/UDP checksum should stop at the end of the packet or at least after the headers. */
    if (RT_UNLIKELY( pCtx->tu.u16CSE > 0 && pCtx->tu.u16CSE <= pCtx->dw3.u8HDRLEN ))
    {
        E1kLog(("e1kSetupGsoCtx: TUCSE=%#x HDRLEN=%#x\n", pCtx->tu.u16CSE, pCtx->dw3.u8HDRLEN));
        return;
    }

    /* IPv4 checksum offset. */
    if (RT_UNLIKELY( pCtx->dw2.fIP && (size_t)pCtx->ip.u8CSO - pCtx->ip.u8CSS != RT_UOFFSETOF(RTNETIPV4, ip_sum) ))
    {
        E1kLog(("e1kSetupGsoCtx: IPCSO=%#x IPCSS=%#x\n", pCtx->ip.u8CSO, pCtx->ip.u8CSS));
        return;
    }

    /* TCP/UDP checksum offsets. */
    if (RT_UNLIKELY(   (size_t)pCtx->tu.u8CSO - pCtx->tu.u8CSS
                    != ( pCtx->dw2.fTCP
                         ? RT_UOFFSETOF(RTNETTCP, th_sum)
                         : RT_UOFFSETOF(RTNETUDP, uh_sum) ) ))
    {
        E1kLog(("e1kSetupGsoCtx: TUCSO=%#x TUCSS=%#x TCP=%d\n", pCtx->ip.u8CSO, pCtx->ip.u8CSS, pCtx->dw2.fTCP));
        return;
    }

    /*
     * Because of internal networking using a 16-bit size field for GSO context
     * plus frame, we have to make sure we don't exceed this.
     */
    if (RT_UNLIKELY( pCtx->dw3.u8HDRLEN + pCtx->dw2.u20PAYLEN > VBOX_MAX_GSO_SIZE ))
    {
        E1kLog(("e1kSetupGsoCtx: HDRLEN(=%#x) + PAYLEN(=%#x) = %#x, max is %#x\n",
                pCtx->dw3.u8HDRLEN, pCtx->dw2.u20PAYLEN, pCtx->dw3.u8HDRLEN + pCtx->dw2.u20PAYLEN, VBOX_MAX_GSO_SIZE));
        return;
    }

    /*
     * We're good for now - we'll do more checks when seeing the data.
     * So, figure the type of offloading and setup the context.
     */
    if (pCtx->dw2.fIP)
    {
        if (pCtx->dw2.fTCP)
            pGso->u8Type = PDMNETWORKGSOTYPE_IPV4_TCP;
        else
            pGso->u8Type = PDMNETWORKGSOTYPE_IPV4_UDP;
        /** @todo Detect IPv4-IPv6 tunneling (need test setup since linux doesn't do
         *        this yet it seems)... */
    }
    else
    {
        if (pCtx->dw2.fTCP)
            pGso->u8Type = PDMNETWORKGSOTYPE_IPV6_TCP;
        else
            pGso->u8Type = PDMNETWORKGSOTYPE_IPV6_UDP;
    }
    pGso->offHdr1  = pCtx->ip.u8CSS;
    pGso->offHdr2  = pCtx->tu.u8CSS;
    pGso->cbHdrs   = pCtx->dw3.u8HDRLEN;
    pGso->cbMaxSeg = pCtx->dw3.u16MSS;
    Assert(PDMNetGsoIsValid(pGso, sizeof(*pGso), pGso->cbMaxSeg * 5));
    E1kLog2(("e1kSetupGsoCtx: mss=%#x hdr=%#x hdr1=%#x hdr2=%#x %s\n",
             pGso->cbMaxSeg, pGso->cbHdrs, pGso->offHdr1, pGso->offHdr2, PDMNetGsoTypeName((PDMNETWORKGSOTYPE)pGso->u8Type) ));
}

/**
 * Checks if we can use GSO processing for the current TSE frame.
 *
 * @param   pGso                The GSO context.
 * @param   pData               The first data descriptor of the frame.
 * @param   pCtx                The TSO context descriptor.
 */
DECLINLINE(bool) e1kCanDoGso(PCPDMNETWORKGSO pGso, E1KTXDAT const *pData, E1KTXCTX const *pCtx)
{
    if (!pData->cmd.fTSE)
    {
        E1kLog2(("e1kCanDoGso: !TSE\n"));
        return false;
    }
    if (pData->cmd.fVLE) /** @todo VLAN tagging. */
    {
        E1kLog(("e1kCanDoGso: VLE\n"));
        return false;
    }

    switch ((PDMNETWORKGSOTYPE)pGso->u8Type)
    {
        case PDMNETWORKGSOTYPE_IPV4_TCP:
        case PDMNETWORKGSOTYPE_IPV4_UDP:
            if (!pData->dw3.fIXSM)
            {
                E1kLog(("e1kCanDoGso: !IXSM (IPv4)\n"));
                return false;
            }
            if (!pData->dw3.fTXSM)
            {
                E1kLog(("e1kCanDoGso: !TXSM (IPv4)\n"));
                return false;
            }
            /** @todo what more check should we perform here? Ethernet frame type? */
            E1kLog2(("e1kCanDoGso: OK, IPv4\n"));
            return true;

        case PDMNETWORKGSOTYPE_IPV6_TCP:
        case PDMNETWORKGSOTYPE_IPV6_UDP:
            if (pData->dw3.fIXSM && pCtx->ip.u8CSO)
            {
                E1kLog(("e1kCanDoGso: IXSM (IPv6)\n"));
                return false;
            }
            if (!pData->dw3.fTXSM)
            {
                E1kLog(("e1kCanDoGso: TXSM (IPv6)\n"));
                return false;
            }
            /** @todo what more check should we perform here? Ethernet frame type? */
            E1kLog2(("e1kCanDoGso: OK, IPv4\n"));
            return true;

        default:
            Assert(pGso->u8Type == PDMNETWORKGSOTYPE_INVALID);
            E1kLog2(("e1kCanDoGso: e1kSetupGsoCtx failed\n"));
            return false;
    }
}

/**
 * Frees the current xmit buffer.
 *
 * @param   pState              The device state structure.
 */
static void e1kXmitFreeBuf(E1KSTATE *pState)
{
    PPDMSCATTERGATHER pSg = pState->CTX_SUFF(pTxSg);
    if (pSg)
    {
        pState->CTX_SUFF(pTxSg) = NULL;

        if (pSg->pvAllocator != pState)
        {
            PPDMINETWORKUP pDrv = pState->CTX_SUFF(pDrv);
            if (pDrv)
                pDrv->pfnFreeBuf(pDrv, pSg);
        }
        else
        {
            /* loopback */
            AssertCompileMemberSize(E1KSTATE, uTxFallback.Sg, 8 * sizeof(size_t));
            Assert(pSg->fFlags == (PDMSCATTERGATHER_FLAGS_MAGIC | PDMSCATTERGATHER_FLAGS_OWNER_3));
            pSg->fFlags = 0;
            pSg->pvAllocator = NULL;
        }
    }
}

/**
 * Allocates a xmit buffer.
 *
 * Presently this will always return a buffer.  Later on we'll have a
 * out-of-buffer mechanism in place where the driver calls us back when buffers
 * becomes available.
 *
 * @returns See PDMINETWORKUP::pfnAllocBuf.
 * @param   pState              The device state structure.
 * @param   cbMin               The minimum frame size.
 * @param   fExactSize          Whether cbMin is exact or if we have to max it
 *                              out to the max MTU size.
 * @param   fGso                Whether this is a GSO frame or not.
 */
DECLINLINE(int) e1kXmitAllocBuf(E1KSTATE *pState, size_t cbMin, bool fExactSize, bool fGso)
{
    /* Adjust cbMin if necessary. */
    if (!fExactSize)
        cbMin = RT_MAX(cbMin, E1K_MAX_TX_PKT_SIZE);

    /* Deal with existing buffer (descriptor screw up, reset, etc). */
    if (RT_UNLIKELY(pState->CTX_SUFF(pTxSg)))
        e1kXmitFreeBuf(pState);
    Assert(pState->CTX_SUFF(pTxSg) == NULL);

    /*
     * Allocate the buffer.
     */
    PPDMSCATTERGATHER pSg;
    if (RT_LIKELY(GET_BITS(RCTL, LBM) != RCTL_LBM_TCVR))
    {
        PPDMINETWORKUP pDrv = pState->CTX_SUFF(pDrv);
        if (RT_UNLIKELY(!pDrv))
            return VERR_NET_DOWN;
        int rc = pDrv->pfnAllocBuf(pDrv, cbMin, fGso ? &pState->GsoCtx : NULL, &pSg);
        if (RT_FAILURE(rc))
            return rc;
    }
    else
    {
        /* Create a loopback using the fallback buffer and preallocated SG. */
        AssertCompileMemberSize(E1KSTATE, uTxFallback.Sg, 8 * sizeof(size_t));
        pSg = &pState->uTxFallback.Sg;
        pSg->fFlags      = PDMSCATTERGATHER_FLAGS_MAGIC | PDMSCATTERGATHER_FLAGS_OWNER_3;
        pSg->cbUsed      = 0;
        pSg->cbAvailable = 0;
        pSg->pvAllocator = pState;
        pSg->pvUser      = NULL; /* No GSO here. */
        pSg->cSegs       = 1;
        pSg->aSegs[0].pvSeg = pState->aTxPacketFallback;
        pSg->aSegs[0].cbSeg = sizeof(pState->aTxPacketFallback);
    }

    pState->CTX_SUFF(pTxSg) = pSg;
    return VINF_SUCCESS;
}

/**
 * Checks if it's a GSO buffer or not.
 *
 * @returns true / false.
 * @param   pTxSg               The scatter / gather buffer.
 */
DECLINLINE(bool) e1kXmitIsGsoBuf(PDMSCATTERGATHER const *pTxSg)
{
#if 0
    if (!pTxSg)
        E1kLog(("e1kXmitIsGsoBuf: pTxSG is NULL\n"));
    if (pTxSg && pTxSg->pvUser)
        E1kLog(("e1kXmitIsGsoBuf: pvUser is NULL\n"));
#endif
    return pTxSg && pTxSg->pvUser /* GSO indicator */;
}

/**
 * Load transmit descriptor from guest memory.
 *
 * @param   pState      The device state structure.
 * @param   pDesc       Pointer to descriptor union.
 * @param   addr        Physical address in guest context.
 * @thread  E1000_TX
 */
DECLINLINE(void) e1kLoadDesc(E1KSTATE* pState, E1KTXDESC* pDesc, RTGCPHYS addr)
{
    PDMDevHlpPhysRead(pState->CTX_SUFF(pDevIns), addr, pDesc, sizeof(E1KTXDESC));
}

/**
 * Write back transmit descriptor to guest memory.
 *
 * @param   pState      The device state structure.
 * @param   pDesc       Pointer to descriptor union.
 * @param   addr        Physical address in guest context.
 * @thread  E1000_TX
 */
DECLINLINE(void) e1kWriteBackDesc(E1KSTATE* pState, E1KTXDESC* pDesc, RTGCPHYS addr)
{
    /* Only the last half of the descriptor has to be written back. */
    e1kPrintTDesc(pState, pDesc, "^^^");
    PDMDevHlpPhysWrite(pState->CTX_SUFF(pDevIns), addr, pDesc, sizeof(E1KTXDESC));
}

/**
 * Transmit complete frame.
 *
 * @remarks We skip the FCS since we're not responsible for sending anything to
 *          a real ethernet wire.
 *
 * @param   pState              The device state structure.
 * @param   fOnWorkerThread     Whether we're on a worker thread or an EMT.
 * @thread  E1000_TX
 */
static void e1kTransmitFrame(E1KSTATE* pState, bool fOnWorkerThread)
{
    PPDMSCATTERGATHER   pSg     = pState->CTX_SUFF(pTxSg);
    uint32_t const      cbFrame = pSg ? (uint32_t)pSg->cbUsed : 0;
    Assert(!pSg || pSg->cSegs == 1);

/*    E1kLog2(("%s <<< Outgoing packet. Dump follows: >>>\n"
            "%.*Rhxd\n"
            "%s <<<<<<<<<<<<< End of dump >>>>>>>>>>>>\n",
            INSTANCE(pState), cbFrame, pSg->aSegs[0].pvSeg, INSTANCE(pState)));*/

    if (cbFrame > 70) /* unqualified guess */
        pState->led.Asserted.s.fWriting = pState->led.Actual.s.fWriting = 1;

    /* Update the stats */
    E1K_INC_CNT32(TPT);
    E1K_ADD_CNT64(TOTL, TOTH, cbFrame);
    E1K_INC_CNT32(GPTC);
    if (pSg && e1kIsBroadcast(pSg->aSegs[0].pvSeg))
        E1K_INC_CNT32(BPTC);
    else if (pSg && e1kIsMulticast(pSg->aSegs[0].pvSeg))
        E1K_INC_CNT32(MPTC);
    /* Update octet transmit counter */
    E1K_ADD_CNT64(GOTCL, GOTCH, cbFrame);
    if (pState->CTX_SUFF(pDrv))
        STAM_REL_COUNTER_ADD(&pState->StatTransmitBytes, cbFrame);
    if (cbFrame == 64)
        E1K_INC_CNT32(PTC64);
    else if (cbFrame < 128)
        E1K_INC_CNT32(PTC127);
    else if (cbFrame < 256)
        E1K_INC_CNT32(PTC255);
    else if (cbFrame < 512)
        E1K_INC_CNT32(PTC511);
    else if (cbFrame < 1024)
        E1K_INC_CNT32(PTC1023);
    else
        E1K_INC_CNT32(PTC1522);

    E1K_INC_ISTAT_CNT(pState->uStatTxFrm);

    /*
     * Dump and send the packet.
     */
    int rc = VERR_NET_DOWN;
    if (pSg && pSg->pvAllocator != pState)
    {
        e1kPacketDump(pState, (uint8_t const *)pSg->aSegs[0].pvSeg, cbFrame, "--> Outgoing");

        pState->CTX_SUFF(pTxSg) = NULL;
        PPDMINETWORKUP pDrv = pState->CTX_SUFF(pDrv);
        if (pDrv)
        {
            /* Release critical section to avoid deadlock in CanReceive */
            //e1kCsLeave(pState);
            e1kMutexRelease(pState);
            STAM_PROFILE_START(&pState->CTX_SUFF_Z(StatTransmitSend), a);
            rc = pDrv->pfnSendBuf(pDrv, pSg, fOnWorkerThread);
            STAM_PROFILE_STOP(&pState->CTX_SUFF_Z(StatTransmitSend), a);
            e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS);
            //e1kCsEnter(pState, RT_SRC_POS);
        }
    }
    else if (pSg)
    {
        Assert(pSg->aSegs[0].pvSeg == pState->aTxPacketFallback);
        e1kPacketDump(pState, (uint8_t const *)pSg->aSegs[0].pvSeg, cbFrame, "--> Loopback");

        /** @todo do we actually need to check that we're in loopback mode here? */
        if (GET_BITS(RCTL, LBM) == RCTL_LBM_TCVR)
        {
            E1KRXDST status;
            RT_ZERO(status);
            status.fPIF = true;
            e1kHandleRxPacket(pState, pSg->aSegs[0].pvSeg, cbFrame, status);
            rc = VINF_SUCCESS;
        }
        e1kXmitFreeBuf(pState);
    }
    else
        rc = VERR_NET_DOWN;
    if (RT_FAILURE(rc))
    {
        E1kLogRel(("E1000: ERROR! pfnSend returned %Rrc\n", rc));
        /** @todo handle VERR_NET_DOWN and VERR_NET_NO_BUFFER_SPACE. Signal error ? */
    }

    pState->led.Actual.s.fWriting = 0;
}

/**
 * Compute and write internet checksum (e1kCSum16) at the specified offset.
 *
 * @param   pState      The device state structure.
 * @param   pPkt        Pointer to the packet.
 * @param   u16PktLen   Total length of the packet.
 * @param   cso         Offset in packet to write checksum at.
 * @param   css         Offset in packet to start computing
 *                      checksum from.
 * @param   cse         Offset in packet to stop computing
 *                      checksum at.
 * @thread  E1000_TX
 */
static void e1kInsertChecksum(E1KSTATE* pState, uint8_t *pPkt, uint16_t u16PktLen, uint8_t cso, uint8_t css, uint16_t cse)
{
    if (cso > u16PktLen)
    {
        E1kLog2(("%s cso(%X) is greater than packet length(%X), checksum is not inserted\n",
                 INSTANCE(pState), cso, u16PktLen));
        return;
    }

    if (cse == 0)
        cse = u16PktLen - 1;
    uint16_t u16ChkSum = e1kCSum16(pPkt + css, cse - css + 1);
    E1kLog2(("%s Inserting csum: %04X at %02X, old value: %04X\n", INSTANCE(pState),
             u16ChkSum, cso, *(uint16_t*)(pPkt + cso)));
    *(uint16_t*)(pPkt + cso) = u16ChkSum;
}

/**
 * Add a part of descriptor's buffer to transmit frame.
 *
 * @remarks data.u64BufAddr is used unconditionally for both data
 *          and legacy descriptors since it is identical to
 *          legacy.u64BufAddr.
 *
 * @param   pState          The device state structure.
 * @param   pDesc           Pointer to the descriptor to transmit.
 * @param   u16Len          Length of buffer to the end of segment.
 * @param   fSend           Force packet sending.
 * @param   fOnWorkerThread Whether we're on a worker thread or an EMT.
 * @thread  E1000_TX
 */
static void e1kFallbackAddSegment(E1KSTATE* pState, RTGCPHYS PhysAddr, uint16_t u16Len, bool fSend, bool fOnWorkerThread)
{
    /* TCP header being transmitted */
    struct E1kTcpHeader *pTcpHdr = (struct E1kTcpHeader *)
            (pState->aTxPacketFallback + pState->contextTSE.tu.u8CSS);
    /* IP header being transmitted */
    struct E1kIpHeader *pIpHdr = (struct E1kIpHeader *)
            (pState->aTxPacketFallback + pState->contextTSE.ip.u8CSS);

    E1kLog3(("%s e1kFallbackAddSegment: Length=%x, remaining payload=%x, header=%x, send=%RTbool\n",
             INSTANCE(pState), u16Len, pState->u32PayRemain, pState->u16HdrRemain, fSend));
    Assert(pState->u32PayRemain + pState->u16HdrRemain > 0);

    PDMDevHlpPhysRead(pState->CTX_SUFF(pDevIns), PhysAddr,
                      pState->aTxPacketFallback + pState->u16TxPktLen, u16Len);
    E1kLog3(("%s Dump of the segment:\n"
            "%.*Rhxd\n"
            "%s --- End of dump ---\n",
            INSTANCE(pState), u16Len, pState->aTxPacketFallback + pState->u16TxPktLen, INSTANCE(pState)));
    pState->u16TxPktLen += u16Len;
    E1kLog3(("%s e1kFallbackAddSegment: pState->u16TxPktLen=%x\n",
            INSTANCE(pState), pState->u16TxPktLen));
    if (pState->u16HdrRemain > 0)
    {
        /* The header was not complete, check if it is now */
        if (u16Len >= pState->u16HdrRemain)
        {
            /* The rest is payload */
            u16Len -= pState->u16HdrRemain;
            pState->u16HdrRemain = 0;
            /* Save partial checksum and flags */
            pState->u32SavedCsum = pTcpHdr->chksum;
            pState->u16SavedFlags = pTcpHdr->hdrlen_flags;
            /* Clear FIN and PSH flags now and set them only in the last segment */
            pTcpHdr->hdrlen_flags &= ~htons(E1K_TCP_FIN | E1K_TCP_PSH);
        }
        else
        {
            /* Still not */
            pState->u16HdrRemain -= u16Len;
            E1kLog3(("%s e1kFallbackAddSegment: Header is still incomplete, 0x%x bytes remain.\n",
                    INSTANCE(pState), pState->u16HdrRemain));
            return;
        }
    }

    pState->u32PayRemain -= u16Len;

    if (fSend)
    {
        /* Leave ethernet header intact */
        /* IP Total Length = payload + headers - ethernet header */
        pIpHdr->total_len = htons(pState->u16TxPktLen - pState->contextTSE.ip.u8CSS);
        E1kLog3(("%s e1kFallbackAddSegment: End of packet, pIpHdr->total_len=%x\n",
                INSTANCE(pState), ntohs(pIpHdr->total_len)));
        /* Update IP Checksum */
        pIpHdr->chksum = 0;
        e1kInsertChecksum(pState, pState->aTxPacketFallback, pState->u16TxPktLen,
                          pState->contextTSE.ip.u8CSO,
                          pState->contextTSE.ip.u8CSS,
                          pState->contextTSE.ip.u16CSE);

        /* Update TCP flags */
        /* Restore original FIN and PSH flags for the last segment */
        if (pState->u32PayRemain == 0)
        {
            pTcpHdr->hdrlen_flags = pState->u16SavedFlags;
            E1K_INC_CNT32(TSCTC);
        }
        /* Add TCP length to partial pseudo header sum */
        uint32_t csum = pState->u32SavedCsum
                + htons(pState->u16TxPktLen - pState->contextTSE.tu.u8CSS);
        while (csum >> 16)
            csum = (csum >> 16) + (csum & 0xFFFF);
        pTcpHdr->chksum = csum;
        /* Compute final checksum */
        e1kInsertChecksum(pState, pState->aTxPacketFallback, pState->u16TxPktLen,
                          pState->contextTSE.tu.u8CSO,
                          pState->contextTSE.tu.u8CSS,
                          pState->contextTSE.tu.u16CSE);

        /*
         * Transmit it. If we've use the SG already, allocate a new one before
         * we copy of the data.
         */
        if (!pState->CTX_SUFF(pTxSg))
            e1kXmitAllocBuf(pState, pState->u16TxPktLen, true /*fExactSize*/, false /*fGso*/);
        if (pState->CTX_SUFF(pTxSg))
        {
            Assert(pState->u16TxPktLen <= pState->CTX_SUFF(pTxSg)->cbAvailable);
            Assert(pState->CTX_SUFF(pTxSg)->cSegs == 1);
            if (pState->CTX_SUFF(pTxSg)->aSegs[0].pvSeg != pState->aTxPacketFallback)
                memcpy(pState->CTX_SUFF(pTxSg)->aSegs[0].pvSeg, pState->aTxPacketFallback, pState->u16TxPktLen);
            pState->CTX_SUFF(pTxSg)->cbUsed         = pState->u16TxPktLen;
            pState->CTX_SUFF(pTxSg)->aSegs[0].cbSeg = pState->u16TxPktLen;
        }
        e1kTransmitFrame(pState, fOnWorkerThread);

        /* Update Sequence Number */
        pTcpHdr->seqno = htonl(ntohl(pTcpHdr->seqno) + pState->u16TxPktLen
                               - pState->contextTSE.dw3.u8HDRLEN);
        /* Increment IP identification */
        pIpHdr->ident = htons(ntohs(pIpHdr->ident) + 1);
    }
}

/**
 * TCP segmentation offloading fallback: Add descriptor's buffer to transmit
 * frame.
 *
 * We construct the frame in the fallback buffer first and the copy it to the SG
 * buffer before passing it down to the network driver code.
 *
 * @returns true if the frame should be transmitted, false if not.
 *
 * @param   pState          The device state structure.
 * @param   pDesc           Pointer to the descriptor to transmit.
 * @param   cbFragment      Length of descriptor's buffer.
 * @param   fOnWorkerThread Whether we're on a worker thread or an EMT.
 * @thread  E1000_TX
 */
static bool e1kFallbackAddToFrame(E1KSTATE* pState, E1KTXDESC* pDesc, uint32_t cbFragment, bool fOnWorkerThread)
{
    PPDMSCATTERGATHER pTxSg = pState->CTX_SUFF(pTxSg);
    Assert(e1kGetDescType(pDesc) == E1K_DTYP_DATA);
    Assert(pDesc->data.cmd.fTSE);
    Assert(!e1kXmitIsGsoBuf(pTxSg));

    uint16_t u16MaxPktLen = pState->contextTSE.dw3.u8HDRLEN + pState->contextTSE.dw3.u16MSS;
    Assert(u16MaxPktLen != 0);
    Assert(u16MaxPktLen < E1K_MAX_TX_PKT_SIZE);

    /*
     * Carve out segments.
     */
    do
    {
        /* Calculate how many bytes we have left in this TCP segment */
        uint32_t cb = u16MaxPktLen - pState->u16TxPktLen;
        if (cb > cbFragment)
        {
            /* This descriptor fits completely into current segment */
            cb = cbFragment;
            e1kFallbackAddSegment(pState, pDesc->data.u64BufAddr, cb, pDesc->data.cmd.fEOP /*fSend*/, fOnWorkerThread);
        }
        else
        {
            e1kFallbackAddSegment(pState, pDesc->data.u64BufAddr, cb, true /*fSend*/, fOnWorkerThread);
            /*
             * Rewind the packet tail pointer to the beginning of payload,
             * so we continue writing right beyond the header.
             */
            pState->u16TxPktLen = pState->contextTSE.dw3.u8HDRLEN;
        }

        pDesc->data.u64BufAddr += cb;
        cbFragment             -= cb;
    } while (cbFragment > 0);

    if (pDesc->data.cmd.fEOP)
    {
        /* End of packet, next segment will contain header. */
        if (pState->u32PayRemain != 0)
            E1K_INC_CNT32(TSCTFC);
        pState->u16TxPktLen = 0;
        e1kXmitFreeBuf(pState);
    }

    return false;
}


/**
 * Add descriptor's buffer to transmit frame.
 *
 * This deals with GSO and normal frames, e1kFallbackAddToFrame deals with the
 * TSE frames we cannot handle as GSO.
 *
 * @returns true on success, false on failure.
 *
 * @param   pThis       The device state structure.
 * @param   PhysAddr    The physical address of the descriptor buffer.
 * @param   cbFragment  Length of descriptor's buffer.
 * @thread  E1000_TX
 */
static bool e1kAddToFrame(E1KSTATE *pThis, RTGCPHYS PhysAddr, uint32_t cbFragment)
{
    PPDMSCATTERGATHER   pTxSg    = pThis->CTX_SUFF(pTxSg);
    bool const          fGso     = e1kXmitIsGsoBuf(pTxSg);
    uint32_t const      cbNewPkt = cbFragment + pThis->u16TxPktLen;

    if (RT_UNLIKELY( !fGso && cbNewPkt > E1K_MAX_TX_PKT_SIZE ))
    {
        E1kLog(("%s Transmit packet is too large: %u > %u(max)\n", INSTANCE(pThis), cbNewPkt, E1K_MAX_TX_PKT_SIZE));
        return false;
    }
    if (RT_UNLIKELY( fGso && cbNewPkt > pTxSg->cbAvailable ))
    {
        E1kLog(("%s Transmit packet is too large: %u > %u(max)/GSO\n", INSTANCE(pThis), cbNewPkt, pTxSg->cbAvailable));
        return false;
    }

    if (RT_LIKELY(pTxSg))
    {
        Assert(pTxSg->cSegs == 1);
        Assert(pTxSg->cbUsed == pThis->u16TxPktLen);

        PDMDevHlpPhysRead(pThis->CTX_SUFF(pDevIns), PhysAddr,
                          (uint8_t *)pTxSg->aSegs[0].pvSeg + pThis->u16TxPktLen, cbFragment);

        pTxSg->cbUsed = cbNewPkt;
    }
    pThis->u16TxPktLen = cbNewPkt;

    return true;
}


/**
 * Write the descriptor back to guest memory and notify the guest.
 *
 * @param   pState      The device state structure.
 * @param   pDesc       Pointer to the descriptor have been transmitted.
 * @param   addr        Physical address of the descriptor in guest memory.
 * @thread  E1000_TX
 */
static void e1kDescReport(E1KSTATE* pState, E1KTXDESC* pDesc, RTGCPHYS addr)
{
    /*
     * We fake descriptor write-back bursting. Descriptors are written back as they are
     * processed.
     */
    /* Let's pretend we process descriptors. Write back with DD set. */
    /*
     * Prior to r71586 we tried to accomodate the case when write-back bursts
     * are enabled without actually implementing bursting by writing back all
     * descriptors, even the ones that do not have RS set. This caused kernel
     * panics with Linux SMP kernels, as the e1000 driver tried to free up skb
     * associated with written back descriptor if it happened to be a context
     * descriptor since context descriptors do not have skb associated to them.
     * Starting from r71586 we write back only the descriptors with RS set,
     * which is a little bit different from what the real hardware does in
     * case there is a chain of data descritors where some of them have RS set
     * and others do not. It is very uncommon scenario imho.
     */
    if (pDesc->legacy.cmd.fRS)
    {
        pDesc->legacy.dw3.fDD = 1; /* Descriptor Done */
        e1kWriteBackDesc(pState, pDesc, addr);
        if (pDesc->legacy.cmd.fEOP)
        {
#ifdef E1K_USE_TX_TIMERS
            if (pDesc->legacy.cmd.fIDE)
            {
                E1K_INC_ISTAT_CNT(pState->uStatTxIDE);
                //if (pState->fIntRaised)
                //{
                //    /* Interrupt is already pending, no need for timers */
                //    ICR |= ICR_TXDW;
                //}
                //else {
                /* Arm the timer to fire in TIVD usec (discard .024) */
                e1kArmTimer(pState, pState->CTX_SUFF(pTIDTimer), TIDV);
# ifndef E1K_NO_TAD
                /* If absolute timer delay is enabled and the timer is not running yet, arm it. */
                E1kLog2(("%s Checking if TAD timer is running\n",
                         INSTANCE(pState)));
                if (TADV != 0 && !TMTimerIsActive(pState->CTX_SUFF(pTADTimer)))
                    e1kArmTimer(pState, pState->CTX_SUFF(pTADTimer), TADV);
# endif /* E1K_NO_TAD */
            }
            else
            {
                E1kLog2(("%s No IDE set, cancel TAD timer and raise interrupt\n",
                        INSTANCE(pState)));
# ifndef E1K_NO_TAD
                /* Cancel both timers if armed and fire immediately. */
                e1kCancelTimer(pState, pState->CTX_SUFF(pTADTimer));
# endif /* E1K_NO_TAD */
#endif /* E1K_USE_TX_TIMERS */
                E1K_INC_ISTAT_CNT(pState->uStatIntTx);
                e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_TXDW);
#ifdef E1K_USE_TX_TIMERS
            }
#endif /* E1K_USE_TX_TIMERS */
        }
    }
    else
    {
        E1K_INC_ISTAT_CNT(pState->uStatTxNoRS);
    }
}

/**
 * Process Transmit Descriptor.
 *
 * E1000 supports three types of transmit descriptors:
 * - legacy   data descriptors of older format (context-less).
 * - data     the same as legacy but providing new offloading capabilities.
 * - context  sets up the context for following data descriptors.
 *
 * @param   pState          The device state structure.
 * @param   pDesc           Pointer to descriptor union.
 * @param   addr            Physical address of descriptor in guest memory.
 * @param   fOnWorkerThread Whether we're on a worker thread or an EMT.
 * @thread  E1000_TX
 */
static void e1kXmitDesc(E1KSTATE* pState, E1KTXDESC* pDesc, RTGCPHYS addr, bool fOnWorkerThread)
{
    e1kPrintTDesc(pState, pDesc, "vvv");

#ifdef E1K_USE_TX_TIMERS
    e1kCancelTimer(pState, pState->CTX_SUFF(pTIDTimer));
#endif /* E1K_USE_TX_TIMERS */

    switch (e1kGetDescType(pDesc))
    {
        case E1K_DTYP_CONTEXT:
            if (pDesc->context.dw2.fTSE)
            {
                pState->contextTSE = pDesc->context;
                pState->u32PayRemain = pDesc->context.dw2.u20PAYLEN;
                pState->u16HdrRemain = pDesc->context.dw3.u8HDRLEN;
                e1kSetupGsoCtx(&pState->GsoCtx, &pDesc->context);
                STAM_COUNTER_INC(&pState->StatTxDescCtxTSE);
            }
            else
            {
                pState->contextNormal = pDesc->context;
                STAM_COUNTER_INC(&pState->StatTxDescCtxNormal);
            }
            E1kLog2(("%s %s context updated: IP CSS=%02X, IP CSO=%02X, IP CSE=%04X"
                    ", TU CSS=%02X, TU CSO=%02X, TU CSE=%04X\n", INSTANCE(pState),
                     pDesc->context.dw2.fTSE ? "TSE" : "Normal",
                     pDesc->context.ip.u8CSS,
                     pDesc->context.ip.u8CSO,
                     pDesc->context.ip.u16CSE,
                     pDesc->context.tu.u8CSS,
                     pDesc->context.tu.u8CSO,
                     pDesc->context.tu.u16CSE));
            E1K_INC_ISTAT_CNT(pState->uStatDescCtx);
            e1kDescReport(pState, pDesc, addr);
            break;

        case E1K_DTYP_DATA:
        {
            if (pDesc->data.cmd.u20DTALEN == 0 || pDesc->data.u64BufAddr == 0)
            {
                E1kLog2(("% Empty data descriptor, skipped.\n", INSTANCE(pState)));
                /** @todo Same as legacy when !TSE. See below. */
                break;
            }
            STAM_COUNTER_INC(pDesc->data.cmd.fTSE?
                             &pState->StatTxDescTSEData:
                             &pState->StatTxDescData);
            STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatTransmit), a);
            E1K_INC_ISTAT_CNT(pState->uStatDescDat);

            /*
             * First fragment: Allocate new buffer and save the IXSM and TXSM
             * packet options as these are only valid in the first fragment.
             */
            if (pState->u16TxPktLen == 0)
            {
                pState->fIPcsum  = pDesc->data.dw3.fIXSM;
                pState->fTCPcsum = pDesc->data.dw3.fTXSM;
                E1kLog2(("%s Saving checksum flags:%s%s; \n", INSTANCE(pState),
                         pState->fIPcsum ? " IP" : "",
                         pState->fTCPcsum ? " TCP/UDP" : ""));
                if (e1kCanDoGso(&pState->GsoCtx, &pDesc->data, &pState->contextTSE))
                    e1kXmitAllocBuf(pState, pState->contextTSE.dw2.u20PAYLEN + pState->contextTSE.dw3.u8HDRLEN,
                                    true /*fExactSize*/, true /*fGso*/);
                else
                    e1kXmitAllocBuf(pState, pState->contextTSE.dw3.u16MSS + pState->contextTSE.dw3.u8HDRLEN,
                                    pDesc->data.cmd.fTSE  /*fExactSize*/, false /*fGso*/);
                /** @todo Is there any way to indicating errors other than collisions? Like
                 *        VERR_NET_DOWN. */
            }

            /*
             * Add the descriptor data to the frame.  If the frame is complete,
             * transmit it and reset the u16TxPktLen field.
             */
            if (e1kXmitIsGsoBuf(pState->CTX_SUFF(pTxSg)))
            {
                STAM_COUNTER_INC(&pState->StatTxPathGSO);
                bool fRc = e1kAddToFrame(pState, pDesc->data.u64BufAddr, pDesc->data.cmd.u20DTALEN);
                if (pDesc->data.cmd.fEOP)
                {
                    if (   fRc
                        && pState->CTX_SUFF(pTxSg)
                        && pState->CTX_SUFF(pTxSg)->cbUsed == (size_t)pState->contextTSE.dw3.u8HDRLEN + pState->contextTSE.dw2.u20PAYLEN)
                    {
                        e1kTransmitFrame(pState, fOnWorkerThread);
                        E1K_INC_CNT32(TSCTC);
                    }
                    else
                    {
                        if (fRc)
                           E1kLog(("%s bad GSO/TSE %p or %u < %u\n" , INSTANCE(pState),
                                   pState->CTX_SUFF(pTxSg), pState->CTX_SUFF(pTxSg) ? pState->CTX_SUFF(pTxSg)->cbUsed : 0,
                                   pState->contextTSE.dw3.u8HDRLEN + pState->contextTSE.dw2.u20PAYLEN));
                        e1kXmitFreeBuf(pState);
                        E1K_INC_CNT32(TSCTFC);
                    }
                    pState->u16TxPktLen = 0;
                }
            }
            else if (!pDesc->data.cmd.fTSE)
            {
                STAM_COUNTER_INC(&pState->StatTxPathRegular);
                bool fRc = e1kAddToFrame(pState, pDesc->data.u64BufAddr, pDesc->data.cmd.u20DTALEN);
                if (pDesc->data.cmd.fEOP)
                {
                    if (fRc && pState->CTX_SUFF(pTxSg))
                    {
                        Assert(pState->CTX_SUFF(pTxSg)->cSegs == 1);
                        if (pState->fIPcsum)
                            e1kInsertChecksum(pState, (uint8_t *)pState->CTX_SUFF(pTxSg)->aSegs[0].pvSeg, pState->u16TxPktLen,
                                              pState->contextNormal.ip.u8CSO,
                                              pState->contextNormal.ip.u8CSS,
                                              pState->contextNormal.ip.u16CSE);
                        if (pState->fTCPcsum)
                            e1kInsertChecksum(pState, (uint8_t *)pState->CTX_SUFF(pTxSg)->aSegs[0].pvSeg, pState->u16TxPktLen,
                                              pState->contextNormal.tu.u8CSO,
                                              pState->contextNormal.tu.u8CSS,
                                              pState->contextNormal.tu.u16CSE);
                        e1kTransmitFrame(pState, fOnWorkerThread);
                    }
                    else
                        e1kXmitFreeBuf(pState);
                    pState->u16TxPktLen = 0;
                }
            }
            else
            {
                STAM_COUNTER_INC(&pState->StatTxPathFallback);
                e1kFallbackAddToFrame(pState, pDesc, pDesc->data.cmd.u20DTALEN, fOnWorkerThread);
            }

            e1kDescReport(pState, pDesc, addr);
            STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatTransmit), a);
            break;
        }

        case E1K_DTYP_LEGACY:
            if (pDesc->legacy.cmd.u16Length == 0 || pDesc->legacy.u64BufAddr == 0)
            {
                E1kLog(("%s Empty legacy descriptor, skipped.\n", INSTANCE(pState)));
                /** @todo 3.3.3, Length/Buffer Address: RS set -> write DD when processing. */
                break;
            }
            STAM_COUNTER_INC(&pState->StatTxDescLegacy);
            STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatTransmit), a);

            /* First fragment: allocate new buffer. */
            if (pState->u16TxPktLen == 0)
                /** @todo reset status bits? */
                e1kXmitAllocBuf(pState, pDesc->legacy.cmd.u16Length, pDesc->legacy.cmd.fEOP, false /*fGso*/);
                /** @todo Is there any way to indicating errors other than collisions? Like
                 *        VERR_NET_DOWN. */

            /* Add fragment to frame. */
            if (e1kAddToFrame(pState, pDesc->data.u64BufAddr, pDesc->legacy.cmd.u16Length))
            {
                E1K_INC_ISTAT_CNT(pState->uStatDescLeg);

                /* Last fragment: Transmit and reset the packet storage counter.  */
                if (pDesc->legacy.cmd.fEOP)
                {
                    /** @todo Offload processing goes here. */
                    e1kTransmitFrame(pState, fOnWorkerThread);
                    pState->u16TxPktLen = 0;
                }
            }
            /* Last fragment + failure: free the buffer and reset the storage counter. */
            else if (pDesc->legacy.cmd.fEOP)
            {
                e1kXmitFreeBuf(pState);
                pState->u16TxPktLen = 0;
            }

            e1kDescReport(pState, pDesc, addr);
            STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatTransmit), a);
            break;

        default:
            E1kLog(("%s ERROR Unsupported transmit descriptor type: 0x%04x\n",
                    INSTANCE(pState), e1kGetDescType(pDesc)));
            break;
    }
}


/**
 * Transmit pending descriptors.
 *
 * @returns VBox status code.  VERR_TRY_AGAIN is returned if we're busy.
 *
 * @param   pState              The E1000 state.
 * @param   fOnWorkerThread     Whether we're on a worker thread or on an EMT.
 */
static int e1kXmitPending(E1KSTATE *pState, bool fOnWorkerThread)
{
    int rc;

    /*
     * Grab the xmit lock of the driver as well as the E1K device state.
     */
    PPDMINETWORKUP pDrv = pState->CTX_SUFF(pDrv);
    if (pDrv)
    {
        rc = pDrv->pfnBeginXmit(pDrv, fOnWorkerThread);
        if (RT_FAILURE(rc))
            return rc;
    }
    rc = e1kMutexAcquire(pState, VERR_TRY_AGAIN, RT_SRC_POS);
    if (RT_SUCCESS(rc))
    {
        /*
         * Process all pending descriptors.
         * Note! Do not process descriptors in locked state
         */
        while (TDH != TDT && !pState->fLocked)
        {
            E1KTXDESC desc;
            E1kLog3(("%s About to process new TX descriptor at %08x%08x, TDLEN=%08x, TDH=%08x, TDT=%08x\n",
                     INSTANCE(pState), TDBAH, TDBAL + TDH * sizeof(desc), TDLEN, TDH, TDT));

            e1kLoadDesc(pState, &desc, ((uint64_t)TDBAH << 32) + TDBAL + TDH * sizeof(desc));
            e1kXmitDesc(pState, &desc, ((uint64_t)TDBAH << 32) + TDBAL + TDH * sizeof(desc), fOnWorkerThread);
            if (++TDH * sizeof(desc) >= TDLEN)
                TDH = 0;

            if (e1kGetTxLen(pState) <= GET_BITS(TXDCTL, LWTHRESH)*8)
            {
                E1kLog2(("%s Low on transmit descriptors, raise ICR.TXD_LOW, len=%x thresh=%x\n",
                         INSTANCE(pState), e1kGetTxLen(pState), GET_BITS(TXDCTL, LWTHRESH)*8));
                e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_TXD_LOW);
            }

            STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatTransmit), a);
        }

        /// @todo: uncomment: pState->uStatIntTXQE++;
        /// @todo: uncomment: e1kRaiseInterrupt(pState, ICR_TXQE);

        /*
         * Release the locks.
         */
        e1kMutexRelease(pState);
    }
    if (pDrv)
        pDrv->pfnEndXmit(pDrv);
    return rc;
}

#ifdef IN_RING3

/**
 * @interface_method_impl{PDMINETWORKDOWN,pfnXmitPending}
 */
static DECLCALLBACK(void) e1kNetworkDown_XmitPending(PPDMINETWORKDOWN pInterface)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkDown);
    e1kXmitPending(pState, true /*fOnWorkerThread*/);
}

/**
 * Callback for consuming from transmit queue. It gets called in R3 whenever
 * we enqueue something in R0/GC.
 *
 * @returns true
 * @param   pDevIns     Pointer to device instance structure.
 * @param   pItem       Pointer to the element being dequeued (not used).
 * @thread  ???
 */
static DECLCALLBACK(bool) e1kTxQueueConsumer(PPDMDEVINS pDevIns, PPDMQUEUEITEMCORE pItem)
{
    NOREF(pItem);
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    E1kLog2(("%s e1kTxQueueConsumer:\n", INSTANCE(pState)));

    int rc = e1kXmitPending(pState, false /*fOnWorkerThread*/);
    AssertMsg(RT_SUCCESS(rc) || rc == VERR_TRY_AGAIN, ("%Rrc\n", rc));

    return true;
}

/**
 * Handler for the wakeup signaller queue.
 */
static DECLCALLBACK(bool) e1kCanRxQueueConsumer(PPDMDEVINS pDevIns, PPDMQUEUEITEMCORE pItem)
{
    e1kWakeupReceive(pDevIns);
    return true;
}

#endif /* IN_RING3 */

/**
 * Write handler for Transmit Descriptor Tail register.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */
static int e1kRegWriteTDT(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    int rc = e1kCsTxEnter(pState, VINF_IOM_HC_MMIO_WRITE);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    rc = e1kRegWriteDefault(pState, offset, index, value);

    /* All descriptors starting with head and not including tail belong to us. */
    /* Process them. */
    E1kLog2(("%s e1kRegWriteTDT: TDBAL=%08x, TDBAH=%08x, TDLEN=%08x, TDH=%08x, TDT=%08x\n",
            INSTANCE(pState), TDBAL, TDBAH, TDLEN, TDH, TDT));

    /* Ignore TDT writes when the link is down. */
    if (TDH != TDT && (STATUS & STATUS_LU))
    {
        E1kLogRel(("E1000: TDT write: %d descriptors to process\n", e1kGetTxLen(pState)));
        E1kLog(("%s e1kRegWriteTDT: %d descriptors to process, waking up E1000_TX thread\n",
                 INSTANCE(pState), e1kGetTxLen(pState)));
        e1kCsTxLeave(pState);

        /* Transmit pending packets if possible, defer it if we cannot do it
           in the current context. */
# ifndef IN_RING3
        if (!pState->CTX_SUFF(pDrv))
        {
            PPDMQUEUEITEMCORE pItem = PDMQueueAlloc(pState->CTX_SUFF(pTxQueue));
            if (RT_UNLIKELY(pItem))
                PDMQueueInsert(pState->CTX_SUFF(pTxQueue), pItem);
        }
        else
# endif
        {
            rc = e1kXmitPending(pState, false /*fOnWorkerThread*/);
            if (rc == VERR_TRY_AGAIN)
                rc = VINF_SUCCESS;
            AssertRC(rc);
        }
    }
    else
        e1kCsTxLeave(pState);

    return rc;
}

/**
 * Write handler for Multicast Table Array registers.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @thread  EMT
 */
static int e1kRegWriteMTA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset < sizeof(pState->auMTA), VERR_DEV_IO_ERROR);
    pState->auMTA[(offset - s_e1kRegMap[index].offset)/sizeof(pState->auMTA[0])] = value;

    return VINF_SUCCESS;
}

/**
 * Read handler for Multicast Table Array registers.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */
static int e1kRegReadMTA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset< sizeof(pState->auMTA), VERR_DEV_IO_ERROR);
    *pu32Value = pState->auMTA[(offset - s_e1kRegMap[index].offset)/sizeof(pState->auMTA[0])];

    return VINF_SUCCESS;
}

/**
 * Write handler for Receive Address registers.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @thread  EMT
 */
static int e1kRegWriteRA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset < sizeof(pState->aRecAddr.au32), VERR_DEV_IO_ERROR);
    pState->aRecAddr.au32[(offset - s_e1kRegMap[index].offset)/sizeof(pState->aRecAddr.au32[0])] = value;

    return VINF_SUCCESS;
}

/**
 * Read handler for Receive Address registers.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */
static int e1kRegReadRA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset< sizeof(pState->aRecAddr.au32), VERR_DEV_IO_ERROR);
    *pu32Value = pState->aRecAddr.au32[(offset - s_e1kRegMap[index].offset)/sizeof(pState->aRecAddr.au32[0])];

    return VINF_SUCCESS;
}

/**
 * Write handler for VLAN Filter Table Array registers.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @thread  EMT
 */
static int e1kRegWriteVFTA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset < sizeof(pState->auVFTA), VINF_SUCCESS);
    pState->auVFTA[(offset - s_e1kRegMap[index].offset)/sizeof(pState->auVFTA[0])] = value;

    return VINF_SUCCESS;
}

/**
 * Read handler for VLAN Filter Table Array registers.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */
static int e1kRegReadVFTA(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    AssertReturn(offset - s_e1kRegMap[index].offset< sizeof(pState->auVFTA), VERR_DEV_IO_ERROR);
    *pu32Value = pState->auVFTA[(offset - s_e1kRegMap[index].offset)/sizeof(pState->auVFTA[0])];

    return VINF_SUCCESS;
}

/**
 * Read handler for unimplemented registers.
 *
 * Merely reports reads from unimplemented registers.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */

static int e1kRegReadUnimplemented(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    E1kLog(("%s At %08X read (00000000) attempt from unimplemented register %s (%s)\n",
            INSTANCE(pState), offset, s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));
    *pu32Value = 0;

    return VINF_SUCCESS;
}

/**
 * Default register read handler with automatic clear operation.
 *
 * Retrieves the value of register from register array in device state structure.
 * Then resets all bits.
 *
 * @remarks The 'mask' parameter is simply ignored as masking and shifting is
 *          done in the caller.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */

static int e1kRegReadAutoClear(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    AssertReturn(index < E1K_NUM_OF_32BIT_REGS, VERR_DEV_IO_ERROR);
    int rc = e1kRegReadDefault(pState, offset, index, pu32Value);
    pState->auRegs[index] = 0;

    return rc;
}

/**
 * Default register read handler.
 *
 * Retrieves the value of register from register array in device state structure.
 * Bits corresponding to 0s in 'readable' mask will always read as 0s.
 *
 * @remarks The 'mask' parameter is simply ignored as masking and shifting is
 *          done in the caller.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @thread  EMT
 */

static int e1kRegReadDefault(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t *pu32Value)
{
    AssertReturn(index < E1K_NUM_OF_32BIT_REGS, VERR_DEV_IO_ERROR);
    *pu32Value = pState->auRegs[index] & s_e1kRegMap[index].readable;

    return VINF_SUCCESS;
}

/**
 * Write handler for unimplemented registers.
 *
 * Merely reports writes to unimplemented registers.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @thread  EMT
 */

 static int e1kRegWriteUnimplemented(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    E1kLog(("%s At %08X write attempt (%08X) to  unimplemented register %s (%s)\n",
            INSTANCE(pState), offset, value, s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));

    return VINF_SUCCESS;
}

/**
 * Default register write handler.
 *
 * Stores the value to the register array in device state structure. Only bits
 * corresponding to 1s both in 'writable' and 'mask' will be stored.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   offset      Register offset in memory-mapped frame.
 * @param   index       Register index in register array.
 * @param   value       The value to store.
 * @param   mask        Used to implement partial writes (8 and 16-bit).
 * @thread  EMT
 */

static int e1kRegWriteDefault(E1KSTATE* pState, uint32_t offset, uint32_t index, uint32_t value)
{
    AssertReturn(index < E1K_NUM_OF_32BIT_REGS, VERR_DEV_IO_ERROR);
    pState->auRegs[index] = (value & s_e1kRegMap[index].writable) |
            (pState->auRegs[index] & ~s_e1kRegMap[index].writable);

    return VINF_SUCCESS;
}

/**
 * Search register table for matching register.
 *
 * @returns Index in the register table or -1 if not found.
 *
 * @param   pState      The device state structure.
 * @param   uOffset     Register offset in memory-mapped region.
 * @thread  EMT
 */
static int e1kRegLookup(E1KSTATE *pState, uint32_t uOffset)
{
    int index;

    for (index = 0; index < E1K_NUM_OF_REGS; index++)
    {
        if (s_e1kRegMap[index].offset <= uOffset && uOffset < s_e1kRegMap[index].offset + s_e1kRegMap[index].size)
        {
            return index;
        }
    }

    return -1;
}

/**
 * Handle register read operation.
 *
 * Looks up and calls appropriate handler.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   uOffset     Register offset in memory-mapped frame.
 * @param   pv          Where to store the result.
 * @param   cb          Number of bytes to read.
 * @thread  EMT
 */
static int e1kRegRead(E1KSTATE *pState, uint32_t uOffset, void *pv, uint32_t cb)
{
    uint32_t    u32    = 0;
    uint32_t    mask   = 0;
    uint32_t    shift;
    int         rc     = VINF_SUCCESS;
    int         index  = e1kRegLookup(pState, uOffset);
    const char *szInst = INSTANCE(pState);
#ifdef DEBUG
    char        buf[9];
#endif

    /*
     * From the spec:
     * For registers that should be accessed as 32-bit double words, partial writes (less than a 32-bit
     * double word) is ignored. Partial reads return all 32 bits of data regardless of the byte enables.
     */

    /*
     * To be able to write bytes and short word we convert them
     * to properly shifted 32-bit words and masks. The idea is
     * to keep register-specific handlers simple. Most accesses
     * will be 32-bit anyway.
     */
    switch (cb)
    {
        case 1: mask = 0x000000FF; break;
        case 2: mask = 0x0000FFFF; break;
        case 4: mask = 0xFFFFFFFF; break;
        default:
            return PDMDevHlpDBGFStop(pState->CTX_SUFF(pDevIns), RT_SRC_POS,
                                     "%s e1kRegRead: unsupported op size: offset=%#10x cb=%#10x\n",
                                     szInst, uOffset, cb);
    }
    if (index != -1)
    {
        if (s_e1kRegMap[index].readable)
        {
            /* Make the mask correspond to the bits we are about to read. */
            shift = (uOffset - s_e1kRegMap[index].offset) % sizeof(uint32_t) * 8;
            mask <<= shift;
            if (!mask)
                return PDMDevHlpDBGFStop(pState->CTX_SUFF(pDevIns), RT_SRC_POS,
                                         "%s e1kRegRead: Zero mask: offset=%#10x cb=%#10x\n",
                                         szInst, uOffset, cb);
            /*
             * Read it. Pass the mask so the handler knows what has to be read.
             * Mask out irrelevant bits.
             */
#ifdef E1K_GLOBAL_MUTEX
            rc = e1kMutexAcquire(pState, VINF_IOM_HC_MMIO_READ, RT_SRC_POS);
#else
            //rc = e1kCsEnter(pState, VERR_SEM_BUSY, RT_SRC_POS);
#endif
            if (RT_UNLIKELY(rc != VINF_SUCCESS))
                return rc;
            //pState->fDelayInts = false;
            //pState->iStatIntLost += pState->iStatIntLostOne;
            //pState->iStatIntLostOne = 0;
            rc = s_e1kRegMap[index].pfnRead(pState, uOffset & 0xFFFFFFFC, index, &u32);
            u32 &= mask;
            //e1kCsLeave(pState);
            e1kMutexRelease(pState);
            E1kLog2(("%s At %08X read  %s          from %s (%s)\n",
                    szInst, uOffset, e1kU32toHex(u32, mask, buf), s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));
            /* Shift back the result. */
            u32 >>= shift;
        }
        else
        {
            E1kLog(("%s At %08X read (%s) attempt from write-only register %s (%s)\n",
                    szInst, uOffset, e1kU32toHex(u32, mask, buf), s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));
        }
    }
    else
    {
        E1kLog(("%s At %08X read (%s) attempt from non-existing register\n",
                szInst, uOffset, e1kU32toHex(u32, mask, buf)));
    }

    memcpy(pv, &u32, cb);
    return rc;
}

/**
 * Handle register write operation.
 *
 * Looks up and calls appropriate handler.
 *
 * @returns VBox status code.
 *
 * @param   pState      The device state structure.
 * @param   uOffset     Register offset in memory-mapped frame.
 * @param   pv          Where to fetch the value.
 * @param   cb          Number of bytes to write.
 * @thread  EMT
 */
static int e1kRegWrite(E1KSTATE *pState, uint32_t uOffset, void const *pv, unsigned cb)
{
    int         rc     = VINF_SUCCESS;
    int         index  = e1kRegLookup(pState, uOffset);
    uint32_t    u32;

    /*
     * From the spec:
     * For registers that should be accessed as 32-bit double words, partial writes (less than a 32-bit
     * double word) is ignored. Partial reads return all 32 bits of data regardless of the byte enables.
     */

    if (cb != 4)
    {
        E1kLog(("%s e1kRegWrite: Spec violation: unsupported op size: offset=%#10x cb=%#10x, ignored.\n",
                INSTANCE(pState), uOffset, cb));
        return VINF_SUCCESS;
    }
    if (uOffset & 3)
    {
        E1kLog(("%s e1kRegWrite: Spec violation: misaligned offset: %#10x cb=%#10x, ignored.\n",
                INSTANCE(pState), uOffset, cb));
        return VINF_SUCCESS;
    }
    u32 = *(uint32_t*)pv;
    if (index != -1)
    {
        if (s_e1kRegMap[index].writable)
        {
            /*
             * Write it. Pass the mask so the handler knows what has to be written.
             * Mask out irrelevant bits.
             */
            E1kLog2(("%s At %08X write          %08X  to  %s (%s)\n",
                     INSTANCE(pState), uOffset, u32, s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));
#ifdef E1K_GLOBAL_MUTEX
            rc = e1kMutexAcquire(pState, VINF_IOM_HC_MMIO_WRITE, RT_SRC_POS);
#else
            //rc = e1kCsEnter(pState, VERR_SEM_BUSY, RT_SRC_POS);
#endif
            if (RT_UNLIKELY(rc != VINF_SUCCESS))
                return rc;
            //pState->fDelayInts = false;
            //pState->iStatIntLost += pState->iStatIntLostOne;
            //pState->iStatIntLostOne = 0;
            rc = s_e1kRegMap[index].pfnWrite(pState, uOffset, index, u32);
            //e1kCsLeave(pState);
            e1kMutexRelease(pState);
        }
        else
        {
            E1kLog(("%s At %08X write attempt (%08X) to  read-only register %s (%s)\n",
                    INSTANCE(pState), uOffset, u32, s_e1kRegMap[index].abbrev, s_e1kRegMap[index].name));
        }
    }
    else
    {
        E1kLog(("%s At %08X write attempt (%08X) to  non-existing register\n",
                INSTANCE(pState), uOffset, u32));
    }
    return rc;
}

/**
 * I/O handler for memory-mapped read operations.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   GCPhysAddr  Physical address (in GC) where the read starts.
 * @param   pv          Where to store the result.
 * @param   cb          Number of bytes read.
 * @thread  EMT
 */
PDMBOTHCBDECL(int) e1kMMIORead(PPDMDEVINS pDevIns, void *pvUser,
                               RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    NOREF(pvUser);
    E1KSTATE  *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    uint32_t  uOffset = GCPhysAddr - pState->addrMMReg;
    STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatMMIORead), a);

    Assert(uOffset < E1K_MM_SIZE);

    int rc = e1kRegRead(pState, uOffset, pv, cb);
    STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatMMIORead), a);
    return rc;
}

/**
 * Memory mapped I/O Handler for write operations.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   GCPhysAddr  Physical address (in GC) where the read starts.
 * @param   pv          Where to fetch the value.
 * @param   cb          Number of bytes to write.
 * @thread  EMT
 */
PDMBOTHCBDECL(int) e1kMMIOWrite(PPDMDEVINS pDevIns, void *pvUser,
                                RTGCPHYS GCPhysAddr, void const *pv, unsigned cb)
{
    NOREF(pvUser);
    E1KSTATE  *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    uint32_t  uOffset = GCPhysAddr - pState->addrMMReg;
    int       rc;
    STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatMMIOWrite), a);

    Assert(uOffset < E1K_MM_SIZE);
    if (cb != 4)
    {
        E1kLog(("%s e1kMMIOWrite: invalid op size: offset=%#10x cb=%#10x", pDevIns, uOffset, cb));
        rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "e1kMMIOWrite: invalid op size: offset=%#10x cb=%#10x\n", uOffset, cb);
    }
    else
        rc = e1kRegWrite(pState, uOffset, pv, cb);

    STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatMMIOWrite), a);
    return rc;
}

/**
 * Port I/O Handler for IN operations.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      Pointer to the device state structure.
 * @param   port        Port number used for the IN operation.
 * @param   pu32        Where to store the result.
 * @param   cb          Number of bytes read.
 * @thread  EMT
 */
PDMBOTHCBDECL(int) e1kIOPortIn(PPDMDEVINS pDevIns, void *pvUser,
                               RTIOPORT port, uint32_t *pu32, unsigned cb)
{
    E1KSTATE   *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    int         rc     = VINF_SUCCESS;
    const char *szInst = INSTANCE(pState);
    STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatIORead), a);

    port -= pState->addrIOPort;
    if (cb != 4)
    {
        E1kLog(("%s e1kIOPortIn: invalid op size: port=%RTiop cb=%08x", szInst, port, cb));
        rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "%s e1kIOPortIn: invalid op size: port=%RTiop cb=%08x\n", szInst, port, cb);
    }
    else
        switch (port)
        {
            case 0x00: /* IOADDR */
                *pu32 = pState->uSelectedReg;
                E1kLog2(("%s e1kIOPortIn: IOADDR(0), selecting register %#010x, val=%#010x\n", szInst, pState->uSelectedReg, *pu32));
                break;
            case 0x04: /* IODATA */
                rc = e1kRegRead(pState, pState->uSelectedReg, pu32, cb);
                /** @todo wrong return code triggers assertions in the debug build; fix please */
                if (rc == VINF_IOM_HC_MMIO_READ)
                    rc = VINF_IOM_HC_IOPORT_READ;

                E1kLog2(("%s e1kIOPortIn: IODATA(4), reading from selected register %#010x, val=%#010x\n", szInst, pState->uSelectedReg, *pu32));
                break;
            default:
                E1kLog(("%s e1kIOPortIn: invalid port %#010x\n", szInst, port));
        //*pRC = VERR_IOM_IOPORT_UNUSED;
        }

    STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatIORead), a);
    return rc;
}


/**
 * Port I/O Handler for OUT operations.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   Port        Port number used for the IN operation.
 * @param   u32         The value to output.
 * @param   cb          The value size in bytes.
 * @thread  EMT
 */
PDMBOTHCBDECL(int) e1kIOPortOut(PPDMDEVINS pDevIns, void *pvUser,
                                RTIOPORT port, uint32_t u32, unsigned cb)
{
    E1KSTATE   *pState = PDMINS_2_DATA(pDevIns, E1KSTATE *);
    int         rc     = VINF_SUCCESS;
    const char *szInst = INSTANCE(pState);
    STAM_PROFILE_ADV_START(&pState->CTX_SUFF_Z(StatIOWrite), a);

    E1kLog2(("%s e1kIOPortOut: port=%RTiop value=%08x\n", szInst, port, u32));
    if (cb != 4)
    {
        E1kLog(("%s e1kIOPortOut: invalid op size: port=%RTiop cb=%08x\n", szInst, port, cb));
        rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "%s e1kIOPortOut: invalid op size: port=%RTiop cb=%08x\n", szInst, port, cb);
    }
    else
    {
        port -= pState->addrIOPort;
        switch (port)
        {
            case 0x00: /* IOADDR */
                pState->uSelectedReg = u32;
                E1kLog2(("%s e1kIOPortOut: IOADDR(0), selected register %08x\n", szInst, pState->uSelectedReg));
                break;
            case 0x04: /* IODATA */
                E1kLog2(("%s e1kIOPortOut: IODATA(4), writing to selected register %#010x, value=%#010x\n", szInst, pState->uSelectedReg, u32));
                rc = e1kRegWrite(pState, pState->uSelectedReg, &u32, cb);
                /** @todo wrong return code triggers assertions in the debug build; fix please */
                if (rc == VINF_IOM_HC_MMIO_WRITE)
                    rc = VINF_IOM_HC_IOPORT_WRITE;
                break;
            default:
                E1kLog(("%s e1kIOPortOut: invalid port %#010x\n", szInst, port));
                /** @todo Do we need to return an error here?
                 * bird: VINF_SUCCESS is fine for unhandled cases of an OUT handler. (If you're curious
                 *       about the guest code and a bit adventuresome, try rc = PDMDeviceDBGFStop(...);) */
                rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "e1kIOPortOut: invalid port %#010x\n", port);
        }
    }

    STAM_PROFILE_ADV_STOP(&pState->CTX_SUFF_Z(StatIOWrite), a);
    return rc;
}

#ifdef IN_RING3
/**
 * Dump complete device state to log.
 *
 * @param   pState          Pointer to device state.
 */
static void e1kDumpState(E1KSTATE *pState)
{
    for (int i = 0; i<E1K_NUM_OF_32BIT_REGS; ++i)
    {
        E1kLog2(("%s %8.8s = %08x\n", INSTANCE(pState),
                s_e1kRegMap[i].abbrev, pState->auRegs[i]));
    }
#ifdef E1K_INT_STATS
    LogRel(("%s Interrupt attempts: %d\n", INSTANCE(pState), pState->uStatIntTry));
    LogRel(("%s Interrupts raised : %d\n", INSTANCE(pState), pState->uStatInt));
    LogRel(("%s Interrupts lowered: %d\n", INSTANCE(pState), pState->uStatIntLower));
    LogRel(("%s Interrupts delayed: %d\n", INSTANCE(pState), pState->uStatIntDly));
    LogRel(("%s Disabled delayed:   %d\n", INSTANCE(pState), pState->uStatDisDly));
    LogRel(("%s Interrupts skipped: %d\n", INSTANCE(pState), pState->uStatIntSkip));
    LogRel(("%s Masked interrupts : %d\n", INSTANCE(pState), pState->uStatIntMasked));
    LogRel(("%s Early interrupts  : %d\n", INSTANCE(pState), pState->uStatIntEarly));
    LogRel(("%s Late interrupts   : %d\n", INSTANCE(pState), pState->uStatIntLate));
    LogRel(("%s Lost interrupts   : %d\n", INSTANCE(pState), pState->iStatIntLost));
    LogRel(("%s Interrupts by RX  : %d\n", INSTANCE(pState), pState->uStatIntRx));
    LogRel(("%s Interrupts by TX  : %d\n", INSTANCE(pState), pState->uStatIntTx));
    LogRel(("%s Interrupts by ICS : %d\n", INSTANCE(pState), pState->uStatIntICS));
    LogRel(("%s Interrupts by RDTR: %d\n", INSTANCE(pState), pState->uStatIntRDTR));
    LogRel(("%s Interrupts by RDMT: %d\n", INSTANCE(pState), pState->uStatIntRXDMT0));
    LogRel(("%s Interrupts by TXQE: %d\n", INSTANCE(pState), pState->uStatIntTXQE));
    LogRel(("%s TX int delay asked: %d\n", INSTANCE(pState), pState->uStatTxIDE));
    LogRel(("%s TX no report asked: %d\n", INSTANCE(pState), pState->uStatTxNoRS));
    LogRel(("%s TX abs timer expd : %d\n", INSTANCE(pState), pState->uStatTAD));
    LogRel(("%s TX int timer expd : %d\n", INSTANCE(pState), pState->uStatTID));
    LogRel(("%s RX abs timer expd : %d\n", INSTANCE(pState), pState->uStatRAD));
    LogRel(("%s RX int timer expd : %d\n", INSTANCE(pState), pState->uStatRID));
    LogRel(("%s TX CTX descriptors: %d\n", INSTANCE(pState), pState->uStatDescCtx));
    LogRel(("%s TX DAT descriptors: %d\n", INSTANCE(pState), pState->uStatDescDat));
    LogRel(("%s TX LEG descriptors: %d\n", INSTANCE(pState), pState->uStatDescLeg));
    LogRel(("%s Received frames   : %d\n", INSTANCE(pState), pState->uStatRxFrm));
    LogRel(("%s Transmitted frames: %d\n", INSTANCE(pState), pState->uStatTxFrm));
#endif /* E1K_INT_STATS */
}

/**
 * Map PCI I/O region.
 *
 * @return  VBox status code.
 * @param   pPciDev         Pointer to PCI device. Use pPciDev->pDevIns to get the device instance.
 * @param   iRegion         The region number.
 * @param   GCPhysAddress   Physical address of the region. If iType is PCI_ADDRESS_SPACE_IO, this is an
 *                          I/O port, else it's a physical address.
 *                          This address is *NOT* relative to pci_mem_base like earlier!
 * @param   cb              Region size.
 * @param   enmType         One of the PCI_ADDRESS_SPACE_* values.
 * @thread  EMT
 */
static DECLCALLBACK(int) e1kMap(PPCIDEVICE pPciDev, int iRegion,
                                RTGCPHYS GCPhysAddress, uint32_t cb, PCIADDRESSSPACE enmType)
{
    int       rc;
    E1KSTATE *pState = PDMINS_2_DATA(pPciDev->pDevIns, E1KSTATE*);

    switch (enmType)
    {
        case PCI_ADDRESS_SPACE_IO:
            pState->addrIOPort = (RTIOPORT)GCPhysAddress;
            rc = PDMDevHlpIOPortRegister(pPciDev->pDevIns, pState->addrIOPort, cb, 0,
                                         e1kIOPortOut, e1kIOPortIn, NULL, NULL, "E1000");
            if (RT_FAILURE(rc))
                break;
            if (pState->fR0Enabled)
            {
                rc = PDMDevHlpIOPortRegisterR0(pPciDev->pDevIns, pState->addrIOPort, cb, 0,
                                             "e1kIOPortOut", "e1kIOPortIn", NULL, NULL, "E1000");
                if (RT_FAILURE(rc))
                    break;
            }
            if (pState->fGCEnabled)
            {
                rc = PDMDevHlpIOPortRegisterRC(pPciDev->pDevIns, pState->addrIOPort, cb, 0,
                                               "e1kIOPortOut", "e1kIOPortIn", NULL, NULL, "E1000");
            }
            break;
        case PCI_ADDRESS_SPACE_MEM:
            pState->addrMMReg = GCPhysAddress;
            rc = PDMDevHlpMMIORegister(pPciDev->pDevIns, GCPhysAddress, cb, NULL /*pvUser*/,
                                       IOMMMIO_FLAGS_READ_PASSTHRU | IOMMMIO_FLAGS_WRITE_PASSTHRU,
                                       e1kMMIOWrite, e1kMMIORead, "E1000");
            if (pState->fR0Enabled)
            {
                rc = PDMDevHlpMMIORegisterR0(pPciDev->pDevIns, GCPhysAddress, cb, NIL_RTR0PTR /*pvUser*/,
                                             "e1kMMIOWrite", "e1kMMIORead");
                if (RT_FAILURE(rc))
                    break;
            }
            if (pState->fGCEnabled)
            {
                rc = PDMDevHlpMMIORegisterRC(pPciDev->pDevIns, GCPhysAddress, cb, NIL_RTRCPTR /*pvUser*/,
                                             "e1kMMIOWrite", "e1kMMIORead");
            }
            break;
        default:
            /* We should never get here */
            AssertMsgFailed(("Invalid PCI address space param in map callback"));
            rc = VERR_INTERNAL_ERROR;
            break;
    }
    return rc;
}

/**
 * Check if the device can receive data now.
 * This must be called before the pfnRecieve() method is called.
 *
 * @returns Number of bytes the device can receive.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @thread  EMT
 */
static int e1kCanReceive(E1KSTATE *pState)
{
    size_t cb;

    if (RT_UNLIKELY(e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS) != VINF_SUCCESS))
        return VERR_NET_NO_BUFFER_SPACE;
    if (RT_UNLIKELY(e1kCsRxEnter(pState, VERR_SEM_BUSY) != VINF_SUCCESS))
        return VERR_NET_NO_BUFFER_SPACE;

    if (RT_UNLIKELY(RDLEN == sizeof(E1KRXDESC)))
    {
        E1KRXDESC desc;
        PDMDevHlpPhysRead(pState->CTX_SUFF(pDevIns), e1kDescAddr(RDBAH, RDBAL, RDH),
                          &desc, sizeof(desc));
        if (desc.status.fDD)
            cb = 0;
        else
            cb = pState->u16RxBSize;
    }
    else if (RDH < RDT)
        cb = (RDT - RDH) * pState->u16RxBSize;
    else if (RDH > RDT)
        cb = (RDLEN/sizeof(E1KRXDESC) - RDH + RDT) * pState->u16RxBSize;
    else
    {
        cb = 0;
        E1kLogRel(("E1000: OUT of RX descriptors!\n"));
    }
    E1kLog2(("%s e1kCanReceive: at exit RDH=%d RDT=%d RDLEN=%d u16RxBSize=%d cb=%lu\n",
             INSTANCE(pState), RDH, RDT, RDLEN, pState->u16RxBSize, cb));

    e1kCsRxLeave(pState);
    e1kMutexRelease(pState);
    return cb > 0 ? VINF_SUCCESS : VERR_NET_NO_BUFFER_SPACE;
}

/**
 * @interface_method_impl{PDMINETWORKDOWN,pfnWaitReceiveAvail}
 */
static DECLCALLBACK(int) e1kNetworkDown_WaitReceiveAvail(PPDMINETWORKDOWN pInterface, RTMSINTERVAL cMillies)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkDown);
    int rc = e1kCanReceive(pState);

    if (RT_SUCCESS(rc))
        return VINF_SUCCESS;
    if (RT_UNLIKELY(cMillies == 0))
        return VERR_NET_NO_BUFFER_SPACE;

    rc = VERR_INTERRUPTED;
    ASMAtomicXchgBool(&pState->fMaybeOutOfSpace, true);
    STAM_PROFILE_START(&pState->StatRxOverflow, a);
    VMSTATE enmVMState;
    while (RT_LIKELY(   (enmVMState = PDMDevHlpVMState(pState->CTX_SUFF(pDevIns))) == VMSTATE_RUNNING
                     ||  enmVMState == VMSTATE_RUNNING_LS))
    {
        int rc2 = e1kCanReceive(pState);
        if (RT_SUCCESS(rc2))
        {
            rc = VINF_SUCCESS;
            break;
        }
        E1kLogRel(("E1000 e1kNetworkDown_WaitReceiveAvail: waiting cMillies=%u...\n",
                cMillies));
        E1kLog(("%s e1kNetworkDown_WaitReceiveAvail: waiting cMillies=%u...\n",
                INSTANCE(pState), cMillies));
        RTSemEventWait(pState->hEventMoreRxDescAvail, cMillies);
    }
    STAM_PROFILE_STOP(&pState->StatRxOverflow, a);
    ASMAtomicXchgBool(&pState->fMaybeOutOfSpace, false);

    return rc;
}


/**
 * Matches the packet addresses against Receive Address table. Looks for
 * exact matches only.
 *
 * @returns true if address matches.
 * @param   pState          Pointer to the state structure.
 * @param   pvBuf           The ethernet packet.
 * @param   cb              Number of bytes available in the packet.
 * @thread  EMT
 */
static bool e1kPerfectMatch(E1KSTATE *pState, const void *pvBuf)
{
    for (unsigned i = 0; i < RT_ELEMENTS(pState->aRecAddr.array); i++)
    {
        E1KRAELEM* ra = pState->aRecAddr.array + i;

        /* Valid address? */
        if (ra->ctl & RA_CTL_AV)
        {
            Assert((ra->ctl & RA_CTL_AS) < 2);
            //unsigned char *pAddr = (unsigned char*)pvBuf + sizeof(ra->addr)*(ra->ctl & RA_CTL_AS);
            //E1kLog3(("%s Matching %02x:%02x:%02x:%02x:%02x:%02x against %02x:%02x:%02x:%02x:%02x:%02x...\n",
            //         INSTANCE(pState), pAddr[0], pAddr[1], pAddr[2], pAddr[3], pAddr[4], pAddr[5],
            //         ra->addr[0], ra->addr[1], ra->addr[2], ra->addr[3], ra->addr[4], ra->addr[5]));
            /*
             * Address Select:
             * 00b = Destination address
             * 01b = Source address
             * 10b = Reserved
             * 11b = Reserved
             * Since ethernet header is (DA, SA, len) we can use address
             * select as index.
             */
            if (memcmp((char*)pvBuf + sizeof(ra->addr)*(ra->ctl & RA_CTL_AS),
                ra->addr, sizeof(ra->addr)) == 0)
                return true;
        }
    }

    return false;
}

/**
 * Matches the packet addresses against Multicast Table Array.
 *
 * @remarks This is imperfect match since it matches not exact address but
 *          a subset of addresses.
 *
 * @returns true if address matches.
 * @param   pState          Pointer to the state structure.
 * @param   pvBuf           The ethernet packet.
 * @param   cb              Number of bytes available in the packet.
 * @thread  EMT
 */
static bool e1kImperfectMatch(E1KSTATE *pState, const void *pvBuf)
{
    /* Get bits 32..47 of destination address */
    uint16_t u16Bit = ((uint16_t*)pvBuf)[2];

    unsigned offset = GET_BITS(RCTL, MO);
    /*
     * offset means:
     * 00b = bits 36..47
     * 01b = bits 35..46
     * 10b = bits 34..45
     * 11b = bits 32..43
     */
    if (offset < 3)
        u16Bit = u16Bit >> (4 - offset);
    return ASMBitTest(pState->auMTA, u16Bit & 0xFFF);
}

/**
 * Determines if the packet is to be delivered to upper layer. The following
 * filters supported:
 * - Exact Unicast/Multicast
 * - Promiscuous Unicast/Multicast
 * - Multicast
 * - VLAN
 *
 * @returns true if packet is intended for this node.
 * @param   pState          Pointer to the state structure.
 * @param   pvBuf           The ethernet packet.
 * @param   cb              Number of bytes available in the packet.
 * @param   pStatus         Bit field to store status bits.
 * @thread  EMT
 */
static bool e1kAddressFilter(E1KSTATE *pState, const void *pvBuf, size_t cb, E1KRXDST *pStatus)
{
    Assert(cb > 14);
    /* Assume that we fail to pass exact filter. */
    pStatus->fPIF = false;
    pStatus->fVP  = false;
    /* Discard oversized packets */
    if (cb > E1K_MAX_RX_PKT_SIZE)
    {
        E1kLog(("%s ERROR: Incoming packet is too big, cb=%d > max=%d\n",
                INSTANCE(pState), cb, E1K_MAX_RX_PKT_SIZE));
        E1K_INC_CNT32(ROC);
        return false;
    }
    else if (!(RCTL & RCTL_LPE) && cb > 1522)
    {
        /* When long packet reception is disabled packets over 1522 are discarded */
        E1kLog(("%s Discarding incoming packet (LPE=0), cb=%d\n",
                INSTANCE(pState), cb));
        E1K_INC_CNT32(ROC);
        return false;
    }

    /* Broadcast filtering */
    if (e1kIsBroadcast(pvBuf) && (RCTL & RCTL_BAM))
        return true;
    E1kLog2(("%s Packet filter: not a broadcast\n", INSTANCE(pState)));
    if (e1kIsMulticast(pvBuf))
    {
        /* Is multicast promiscuous enabled? */
        if (RCTL & RCTL_MPE)
            return true;
        E1kLog2(("%s Packet filter: no promiscuous multicast\n", INSTANCE(pState)));
        /* Try perfect matches first */
        if (e1kPerfectMatch(pState, pvBuf))
        {
            pStatus->fPIF = true;
            return true;
        }
        E1kLog2(("%s Packet filter: no perfect match\n", INSTANCE(pState)));
        if (e1kImperfectMatch(pState, pvBuf))
            return true;
        E1kLog2(("%s Packet filter: no imperfect match\n", INSTANCE(pState)));
    }
    else {
        /* Is unicast promiscuous enabled? */
        if (RCTL & RCTL_UPE)
            return true;
        E1kLog2(("%s Packet filter: no promiscuous unicast\n", INSTANCE(pState)));
        if (e1kPerfectMatch(pState, pvBuf))
        {
            pStatus->fPIF = true;
            return true;
        }
        E1kLog2(("%s Packet filter: no perfect match\n", INSTANCE(pState)));
    }
    /* Is VLAN filtering enabled? */
    if (RCTL & RCTL_VFE)
    {
        uint16_t *u16Ptr = (uint16_t*)pvBuf;
        /* Compare TPID with VLAN Ether Type */
        if (u16Ptr[6] == VET)
        {
            pStatus->fVP = true;
            /* It is 802.1q packet indeed, let's filter by VID */
            if (ASMBitTest(pState->auVFTA, RT_BE2H_U16(u16Ptr[7]) & 0xFFF))
                return true;
            E1kLog2(("%s Packet filter: no VLAN match\n", INSTANCE(pState)));
        }
    }
    E1kLog2(("%s Packet filter: packet discarded\n", INSTANCE(pState)));
    return false;
}

/**
 * @interface_method_impl{PDMINETWORKDOWN,pfnReceive}
 */
static DECLCALLBACK(int) e1kNetworkDown_Receive(PPDMINETWORKDOWN pInterface, const void *pvBuf, size_t cb)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkDown);
    int       rc = VINF_SUCCESS;

    /*
     * Drop packets if the VM is not running yet/anymore.
     */
    VMSTATE enmVMState = PDMDevHlpVMState(STATE_TO_DEVINS(pState));
    if (    enmVMState != VMSTATE_RUNNING
        &&  enmVMState != VMSTATE_RUNNING_LS)
    {
        E1kLog(("%s Dropping incoming packet as VM is not running.\n", INSTANCE(pState)));
        return VINF_SUCCESS;
    }

    /* Discard incoming packets in locked state */
    if (!(RCTL & RCTL_EN) || pState->fLocked || !(STATUS & STATUS_LU))
    {
        E1kLog(("%s Dropping incoming packet as receive operation is disabled.\n", INSTANCE(pState)));
        return VINF_SUCCESS;
    }

    STAM_PROFILE_ADV_START(&pState->StatReceive, a);
    rc = e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS);
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        //if (!e1kCsEnter(pState, RT_SRC_POS))
        //    return VERR_PERMISSION_DENIED;

        e1kPacketDump(pState, (const uint8_t*)pvBuf, cb, "<-- Incoming");

        /* Update stats */
        if (RT_LIKELY(e1kCsEnter(pState, VERR_SEM_BUSY) == VINF_SUCCESS))
        {
            E1K_INC_CNT32(TPR);
            E1K_ADD_CNT64(TORL, TORH, cb < 64? 64 : cb);
            e1kCsLeave(pState);
        }
        STAM_PROFILE_ADV_START(&pState->StatReceiveFilter, a);
        E1KRXDST status;
        RT_ZERO(status);
        bool fPassed = e1kAddressFilter(pState, pvBuf, cb, &status);
        STAM_PROFILE_ADV_STOP(&pState->StatReceiveFilter, a);
        if (fPassed)
        {
            rc = e1kHandleRxPacket(pState, pvBuf, cb, status);
        }
        //e1kCsLeave(pState);
        e1kMutexRelease(pState);
    }
    STAM_PROFILE_ADV_STOP(&pState->StatReceive, a);

    return rc;
}

/**
 * Gets the pointer to the status LED of a unit.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure.
 * @param   iLUN            The unit which status LED we desire.
 * @param   ppLed           Where to store the LED pointer.
 * @thread  EMT
 */
static DECLCALLBACK(int) e1kQueryStatusLed(PPDMILEDPORTS pInterface, unsigned iLUN, PPDMLED *ppLed)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, ILeds);
    int       rc     = VERR_PDM_LUN_NOT_FOUND;

    if (iLUN == 0)
    {
        *ppLed = &pState->led;
        rc     = VINF_SUCCESS;
    }
    return rc;
}

/**
 * Gets the current Media Access Control (MAC) address.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   pMac            Where to store the MAC address.
 * @thread  EMT
 */
static DECLCALLBACK(int) e1kGetMac(PPDMINETWORKCONFIG pInterface, PRTMAC pMac)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkConfig);
    pState->eeprom.getMac(pMac);
    return VINF_SUCCESS;
}


/**
 * Gets the new link state.
 *
 * @returns The current link state.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @thread  EMT
 */
static DECLCALLBACK(PDMNETWORKLINKSTATE) e1kGetLinkState(PPDMINETWORKCONFIG pInterface)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkConfig);
    if (STATUS & STATUS_LU)
        return PDMNETWORKLINKSTATE_UP;
    return PDMNETWORKLINKSTATE_DOWN;
}


/**
 * Sets the new link state.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   enmState        The new link state
 * @thread  EMT
 */
static DECLCALLBACK(int) e1kSetLinkState(PPDMINETWORKCONFIG pInterface, PDMNETWORKLINKSTATE enmState)
{
    E1KSTATE *pState = RT_FROM_MEMBER(pInterface, E1KSTATE, INetworkConfig);
    bool fOldUp = !!(STATUS & STATUS_LU);
    bool fNewUp = enmState == PDMNETWORKLINKSTATE_UP;

    if (   fNewUp != fOldUp
        || (!fNewUp && pState->fCableConnected)) /* old state was connected but STATUS not
                                                  * yet written by guest */
    {
        if (fNewUp)
        {
            E1kLog(("%s Link will be up in approximately 5 secs\n", INSTANCE(pState)));
            pState->fCableConnected = true;
            STATUS &= ~STATUS_LU;
            Phy::setLinkStatus(&pState->phy, false);
            e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_LSC);
            /* Restore the link back in 5 second. */
            e1kArmTimer(pState, pState->pLUTimerR3, 5000000);
        }
        else
        {
            E1kLog(("%s Link is down\n", INSTANCE(pState)));
            pState->fCableConnected = false;
            STATUS &= ~STATUS_LU;
            Phy::setLinkStatus(&pState->phy, false);
            e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_LSC);
        }
        if (pState->pDrvR3)
            pState->pDrvR3->pfnNotifyLinkChanged(pState->pDrvR3, enmState);
    }
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) e1kQueryInterface(struct PDMIBASE *pInterface, const char *pszIID)
{
    E1KSTATE *pThis = RT_FROM_MEMBER(pInterface, E1KSTATE, IBase);
    Assert(&pThis->IBase == pInterface);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pThis->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMINETWORKDOWN, &pThis->INetworkDown);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMINETWORKCONFIG, &pThis->INetworkConfig);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMILEDPORTS, &pThis->ILeds);
    return NULL;
}

/**
 * Saves the configuration.
 *
 * @param   pState      The E1K state.
 * @param   pSSM        The handle to the saved state.
 */
static void e1kSaveConfig(E1KSTATE *pState, PSSMHANDLE pSSM)
{
    SSMR3PutMem(pSSM, &pState->macConfigured, sizeof(pState->macConfigured));
    SSMR3PutU32(pSSM, pState->eChip);
}

/**
 * Live save - save basic configuration.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 * @param   uPass
 */
static DECLCALLBACK(int) e1kLiveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uPass)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    e1kSaveConfig(pState, pSSM);
    return VINF_SSM_DONT_CALL_AGAIN;
}

/**
 * Prepares for state saving.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 */
static DECLCALLBACK(int) e1kSavePrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);

    int rc = e1kCsEnter(pState, VERR_SEM_BUSY);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    e1kCsLeave(pState);
    return VINF_SUCCESS;
#if 0
    int rc = e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    /* 1) Prevent all threads from modifying the state and memory */
    //pState->fLocked = true;
    /* 2) Cancel all timers */
#ifdef E1K_USE_TX_TIMERS
    e1kCancelTimer(pState, pState->CTX_SUFF(pTIDTimer));
#ifndef E1K_NO_TAD
    e1kCancelTimer(pState, pState->CTX_SUFF(pTADTimer));
#endif /* E1K_NO_TAD */
#endif /* E1K_USE_TX_TIMERS */
#ifdef E1K_USE_RX_TIMERS
    e1kCancelTimer(pState, pState->CTX_SUFF(pRIDTimer));
    e1kCancelTimer(pState, pState->CTX_SUFF(pRADTimer));
#endif /* E1K_USE_RX_TIMERS */
    e1kCancelTimer(pState, pState->CTX_SUFF(pIntTimer));
    /* 3) Did I forget anything? */
    E1kLog(("%s Locked\n", INSTANCE(pState)));
    e1kMutexRelease(pState);
    return VINF_SUCCESS;
#endif
}


/**
 * Saves the state of device.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 */
static DECLCALLBACK(int) e1kSaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);

    e1kSaveConfig(pState, pSSM);
    pState->eeprom.save(pSSM);
    e1kDumpState(pState);
    SSMR3PutMem(pSSM, pState->auRegs, sizeof(pState->auRegs));
    SSMR3PutBool(pSSM, pState->fIntRaised);
    Phy::saveState(pSSM, &pState->phy);
    SSMR3PutU32(pSSM, pState->uSelectedReg);
    SSMR3PutMem(pSSM, pState->auMTA, sizeof(pState->auMTA));
    SSMR3PutMem(pSSM, &pState->aRecAddr, sizeof(pState->aRecAddr));
    SSMR3PutMem(pSSM, pState->auVFTA, sizeof(pState->auVFTA));
    SSMR3PutU64(pSSM, pState->u64AckedAt);
    SSMR3PutU16(pSSM, pState->u16RxBSize);
    //SSMR3PutBool(pSSM, pState->fDelayInts);
    //SSMR3PutBool(pSSM, pState->fIntMaskUsed);
    SSMR3PutU16(pSSM, pState->u16TxPktLen);
/** @todo State wrt to the TSE buffer is incomplete, so little point in
 *        saving this actually. */
    SSMR3PutMem(pSSM, pState->aTxPacketFallback, pState->u16TxPktLen);
    SSMR3PutBool(pSSM, pState->fIPcsum);
    SSMR3PutBool(pSSM, pState->fTCPcsum);
    SSMR3PutMem(pSSM, &pState->contextTSE, sizeof(pState->contextTSE));
    SSMR3PutMem(pSSM, &pState->contextNormal, sizeof(pState->contextNormal));
/**@todo GSO requires some more state here. */
    E1kLog(("%s State has been saved\n", INSTANCE(pState)));
    return VINF_SUCCESS;
}

#if 0
/**
 * Cleanup after saving.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 */
static DECLCALLBACK(int) e1kSaveDone(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);

    int rc = e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    /* If VM is being powered off unlocking will result in assertions in PGM */
    if (PDMDevHlpGetVM(pDevIns)->enmVMState == VMSTATE_RUNNING)
        pState->fLocked = false;
    else
        E1kLog(("%s VM is not running -- remain locked\n", INSTANCE(pState)));
    E1kLog(("%s Unlocked\n", INSTANCE(pState)));
    e1kMutexRelease(pState);
    return VINF_SUCCESS;
}
#endif

/**
 * Sync with .
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 */
static DECLCALLBACK(int) e1kLoadPrep(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);

    int rc = e1kCsEnter(pState, VERR_SEM_BUSY);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;
    e1kCsLeave(pState);
    return VINF_SUCCESS;
}

/**
 * Restore previously saved state of device.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 * @param   uVersion    The data unit version number.
 * @param   uPass       The data pass.
 */
static DECLCALLBACK(int) e1kLoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    int       rc;

    if (    uVersion != E1K_SAVEDSTATE_VERSION
        &&  uVersion != E1K_SAVEDSTATE_VERSION_VBOX_30)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    if (   uVersion > E1K_SAVEDSTATE_VERSION_VBOX_30
        || uPass    != SSM_PASS_FINAL)
    {
        /* config checks */
        RTMAC macConfigured;
        rc = SSMR3GetMem(pSSM, &macConfigured, sizeof(macConfigured));
        AssertRCReturn(rc, rc);
        if (   memcmp(&macConfigured, &pState->macConfigured, sizeof(macConfigured))
            && (uPass == 0 || !PDMDevHlpVMTeleportedAndNotFullyResumedYet(pDevIns)) )
            LogRel(("%s: The mac address differs: config=%RTmac saved=%RTmac\n", INSTANCE(pState), &pState->macConfigured, &macConfigured));

        E1KCHIP eChip;
        rc = SSMR3GetU32(pSSM, &eChip);
        AssertRCReturn(rc, rc);
        if (eChip != pState->eChip)
            return SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("The chip type differs: config=%u saved=%u"), pState->eChip, eChip);
    }

    if (uPass == SSM_PASS_FINAL)
    {
        if (uVersion > E1K_SAVEDSTATE_VERSION_VBOX_30)
        {
            rc = pState->eeprom.load(pSSM);
            AssertRCReturn(rc, rc);
        }
        /* the state */
        SSMR3GetMem(pSSM, &pState->auRegs, sizeof(pState->auRegs));
        SSMR3GetBool(pSSM, &pState->fIntRaised);
        /** @todo: PHY could be made a separate device with its own versioning */
        Phy::loadState(pSSM, &pState->phy);
        SSMR3GetU32(pSSM, &pState->uSelectedReg);
        SSMR3GetMem(pSSM, &pState->auMTA, sizeof(pState->auMTA));
        SSMR3GetMem(pSSM, &pState->aRecAddr, sizeof(pState->aRecAddr));
        SSMR3GetMem(pSSM, &pState->auVFTA, sizeof(pState->auVFTA));
        SSMR3GetU64(pSSM, &pState->u64AckedAt);
        SSMR3GetU16(pSSM, &pState->u16RxBSize);
        //SSMR3GetBool(pSSM, pState->fDelayInts);
        //SSMR3GetBool(pSSM, pState->fIntMaskUsed);
        SSMR3GetU16(pSSM, &pState->u16TxPktLen);
        SSMR3GetMem(pSSM, &pState->aTxPacketFallback[0], pState->u16TxPktLen);
        SSMR3GetBool(pSSM, &pState->fIPcsum);
        SSMR3GetBool(pSSM, &pState->fTCPcsum);
        SSMR3GetMem(pSSM, &pState->contextTSE, sizeof(pState->contextTSE));
        rc = SSMR3GetMem(pSSM, &pState->contextNormal, sizeof(pState->contextNormal));
        AssertRCReturn(rc, rc);

        /* derived state  */
        e1kSetupGsoCtx(&pState->GsoCtx, &pState->contextTSE);

        E1kLog(("%s State has been restored\n", INSTANCE(pState)));
        e1kDumpState(pState);
    }
    return VINF_SUCCESS;
}

/**
 * Link status adjustments after loading.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   pSSM        The handle to the saved state.
 */
static DECLCALLBACK(int) e1kLoadDone(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);

    int rc = e1kMutexAcquire(pState, VERR_SEM_BUSY, RT_SRC_POS);
    if (RT_UNLIKELY(rc != VINF_SUCCESS))
        return rc;

    /* Update promiscuous mode */
    if (pState->pDrvR3)
        pState->pDrvR3->pfnSetPromiscuousMode(pState->pDrvR3,
                                             !!(RCTL & (RCTL_UPE | RCTL_MPE)));

    /*
    * Force the link down here, since PDMNETWORKLINKSTATE_DOWN_RESUME is never
    * passed to us. We go through all this stuff if the link was up and we
    * wasn't teleported.
    */
    if (    (STATUS & STATUS_LU)
        && !PDMDevHlpVMTeleportedAndNotFullyResumedYet(pDevIns))
    {
        E1kLog(("%s Link is down temporarily\n", INSTANCE(pState)));
        STATUS &= ~STATUS_LU;
        Phy::setLinkStatus(&pState->phy, false);
        e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_LSC);
        /* Restore the link back in five seconds. */
        e1kArmTimer(pState, pState->pLUTimerR3, 5000000);
    }
    e1kMutexRelease(pState);
    return VINF_SUCCESS;
}


/* -=-=-=-=- PDMDEVREG -=-=-=-=- */

/**
 * Detach notification.
 *
 * One port on the network card has been disconnected from the network.
 *
 * @param   pDevIns     The device instance.
 * @param   iLUN        The logical unit which is being detached.
 * @param   fFlags      Flags, combination of the PDMDEVATT_FLAGS_* \#defines.
 */
static DECLCALLBACK(void) e1kDetach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    Log(("%s e1kDetach:\n", INSTANCE(pState)));

    AssertLogRelReturnVoid(iLUN == 0);

    PDMCritSectEnter(&pState->cs, VERR_SEM_BUSY);

    /** @todo: r=pritesh still need to check if i missed
     * to clean something in this function
     */

    /*
     * Zero some important members.
     */
    pState->pDrvBase = NULL;
    pState->pDrvR3 = NULL;
    pState->pDrvR0 = NIL_RTR0PTR;
    pState->pDrvRC = NIL_RTRCPTR;

    PDMCritSectLeave(&pState->cs);
}

/**
 * Attach the Network attachment.
 *
 * One port on the network card has been connected to a network.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   iLUN        The logical unit which is being attached.
 * @param   fFlags      Flags, combination of the PDMDEVATT_FLAGS_* \#defines.
 *
 * @remarks This code path is not used during construction.
 */
static DECLCALLBACK(int) e1kAttach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    LogFlow(("%s e1kAttach:\n",  INSTANCE(pState)));

    AssertLogRelReturn(iLUN == 0, VERR_PDM_NO_SUCH_LUN);

    PDMCritSectEnter(&pState->cs, VERR_SEM_BUSY);

    /*
     * Attach the driver.
     */
    int rc = PDMDevHlpDriverAttach(pDevIns, 0, &pState->IBase, &pState->pDrvBase, "Network Port");
    if (RT_SUCCESS(rc))
    {
        if (rc == VINF_NAT_DNS)
        {
#ifdef RT_OS_LINUX
            PDMDevHlpVMSetRuntimeError(pDevIns, 0 /*fFlags*/, "NoDNSforNAT",
                                       N_("A Domain Name Server (DNS) for NAT networking could not be determined. Please check your /etc/resolv.conf for <tt>nameserver</tt> entries. Either add one manually (<i>man resolv.conf</i>) or ensure that your host is correctly connected to an ISP. If you ignore this warning the guest will not be able to perform nameserver lookups and it will probably observe delays if trying so"));
#else
            PDMDevHlpVMSetRuntimeError(pDevIns, 0 /*fFlags*/, "NoDNSforNAT",
                                       N_("A Domain Name Server (DNS) for NAT networking could not be determined. Ensure that your host is correctly connected to an ISP. If you ignore this warning the guest will not be able to perform nameserver lookups and it will probably observe delays if trying so"));
#endif
        }
        pState->pDrvR3 = PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMINETWORKUP);
        AssertMsgStmt(pState->pDrvR3, ("Failed to obtain the PDMINETWORKUP interface!\n"),
                      rc = VERR_PDM_MISSING_INTERFACE_BELOW);
        if (RT_SUCCESS(rc))
        {
            PPDMIBASER0 pBaseR0 = PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMIBASER0);
            pState->pDrvR0 = pBaseR0 ? pBaseR0->pfnQueryInterface(pBaseR0, PDMINETWORKUP_IID) : NIL_RTR0PTR;

            PPDMIBASERC pBaseRC = PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMIBASERC);
            pState->pDrvRC = pBaseRC ? pBaseRC->pfnQueryInterface(pBaseRC, PDMINETWORKUP_IID) : NIL_RTR0PTR;
        }
    }
    else if (   rc == VERR_PDM_NO_ATTACHED_DRIVER
             || rc == VERR_PDM_CFG_MISSING_DRIVER_NAME)
    {
        /* This should never happen because this function is not called
         * if there is no driver to attach! */
        Log(("%s No attached driver!\n", INSTANCE(pState)));
    }

    /*
     * Temporary set the link down if it was up so that the guest
     * will know that we have change the configuration of the
     * network card
     */
    if ((STATUS & STATUS_LU) && RT_SUCCESS(rc))
    {
        STATUS &= ~STATUS_LU;
        Phy::setLinkStatus(&pState->phy, false);
        e1kRaiseInterrupt(pState, VERR_SEM_BUSY, ICR_LSC);
        /* Restore the link back in 5 second. */
        e1kArmTimer(pState, pState->pLUTimerR3, 5000000);
    }

    PDMCritSectLeave(&pState->cs);
    return rc;

}

/**
 * @copydoc FNPDMDEVPOWEROFF
 */
static DECLCALLBACK(void) e1kPowerOff(PPDMDEVINS pDevIns)
{
    /* Poke thread waiting for buffer space. */
    e1kWakeupReceive(pDevIns);
}

/**
 * @copydoc FNPDMDEVRESET
 */
static DECLCALLBACK(void) e1kReset(PPDMDEVINS pDevIns)
{
    E1KSTATE *pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    e1kCancelTimer(pState, pState->CTX_SUFF(pIntTimer));
    e1kCancelTimer(pState, pState->CTX_SUFF(pLUTimer));
    e1kXmitFreeBuf(pState);
    pState->u16TxPktLen  = 0;
    pState->fIPcsum      = false;
    pState->fTCPcsum     = false;
    pState->fIntMaskUsed = false;
    pState->fDelayInts   = false;
    pState->fLocked      = false;
    pState->u64AckedAt   = 0;
    e1kHardReset(pState);
}

/**
 * @copydoc FNPDMDEVSUSPEND
 */
static DECLCALLBACK(void) e1kSuspend(PPDMDEVINS pDevIns)
{
    /* Poke thread waiting for buffer space. */
    e1kWakeupReceive(pDevIns);
}

/**
 * Device relocation callback.
 *
 * When this callback is called the device instance data, and if the
 * device have a GC component, is being relocated, or/and the selectors
 * have been changed. The device must use the chance to perform the
 * necessary pointer relocations and data updates.
 *
 * Before the GC code is executed the first time, this function will be
 * called with a 0 delta so GC pointer calculations can be one in one place.
 *
 * @param   pDevIns     Pointer to the device instance.
 * @param   offDelta    The relocation delta relative to the old location.
 *
 * @remark  A relocation CANNOT fail.
 */
static DECLCALLBACK(void) e1kRelocate(PPDMDEVINS pDevIns, RTGCINTPTR offDelta)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    pState->pDevInsRC     = PDMDEVINS_2_RCPTR(pDevIns);
    pState->pTxQueueRC    = PDMQueueRCPtr(pState->pTxQueueR3);
    pState->pCanRxQueueRC = PDMQueueRCPtr(pState->pCanRxQueueR3);
#ifdef E1K_USE_RX_TIMERS
    pState->pRIDTimerRC   = TMTimerRCPtr(pState->pRIDTimerR3);
    pState->pRADTimerRC   = TMTimerRCPtr(pState->pRADTimerR3);
#endif /* E1K_USE_RX_TIMERS */
#ifdef E1K_USE_TX_TIMERS
    pState->pTIDTimerRC   = TMTimerRCPtr(pState->pTIDTimerR3);
# ifndef E1K_NO_TAD
    pState->pTADTimerRC   = TMTimerRCPtr(pState->pTADTimerR3);
# endif /* E1K_NO_TAD */
#endif /* E1K_USE_TX_TIMERS */
    pState->pIntTimerRC   = TMTimerRCPtr(pState->pIntTimerR3);
    pState->pLUTimerRC    = TMTimerRCPtr(pState->pLUTimerR3);
}

/**
 * Destruct a device instance.
 *
 * We need to free non-VM resources only.
 *
 * @returns VBox status.
 * @param   pDevIns     The device instance data.
 * @thread  EMT
 */
static DECLCALLBACK(int) e1kDestruct(PPDMDEVINS pDevIns)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    PDMDEV_CHECK_VERSIONS_RETURN_QUIET(pDevIns);

    e1kDumpState(pState);
    E1kLog(("%s Destroying instance\n", INSTANCE(pState)));
    if (PDMCritSectIsInitialized(&pState->cs))
    {
        if (pState->hEventMoreRxDescAvail != NIL_RTSEMEVENT)
        {
            RTSemEventSignal(pState->hEventMoreRxDescAvail);
            RTSemEventDestroy(pState->hEventMoreRxDescAvail);
            pState->hEventMoreRxDescAvail = NIL_RTSEMEVENT;
        }
#ifndef E1K_GLOBAL_MUTEX
        PDMR3CritSectDelete(&pState->csRx);
        //PDMR3CritSectDelete(&pState->csTx);
#endif
        PDMR3CritSectDelete(&pState->cs);
    }
    return VINF_SUCCESS;
}

/**
 * Status info callback.
 *
 * @param   pDevIns     The device instance.
 * @param   pHlp        The output helpers.
 * @param   pszArgs     The arguments.
 */
static DECLCALLBACK(void) e1kInfo(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    unsigned    i;
    bool        fRcvRing = false;
    bool        fXmtRing = false;

    /*
     * Parse args.
     */
    if (pszArgs)
    {
        fRcvRing = strstr(pszArgs, "verbose") || strstr(pszArgs, "rcv");
        fXmtRing = strstr(pszArgs, "verbose") || strstr(pszArgs, "xmt");
    }

    /*
     * Show info.
     */
    pHlp->pfnPrintf(pHlp, "E1000 #%d: port=%RTiop mmio=%RX32 mac-cfg=%RTmac %s%s%s\n",
                    pDevIns->iInstance, pState->addrIOPort, pState->addrMMReg,
                    &pState->macConfigured, g_Chips[pState->eChip].pcszName,
                    pState->fGCEnabled ? " GC" : "", pState->fR0Enabled ? " R0" : "");

    e1kCsEnter(pState, VERR_INTERNAL_ERROR); /* Not sure why but PCNet does it */

    for (i = 0; i < E1K_NUM_OF_32BIT_REGS; ++i)
        pHlp->pfnPrintf(pHlp, "%8.8s = %08x\n", s_e1kRegMap[i].abbrev, pState->auRegs[i]);

    for (i = 0; i < RT_ELEMENTS(pState->aRecAddr.array); i++)
    {
        E1KRAELEM* ra = pState->aRecAddr.array + i;
        if (ra->ctl & RA_CTL_AV)
        {
            const char *pcszTmp;
            switch (ra->ctl & RA_CTL_AS)
            {
                case 0:  pcszTmp = "DST"; break;
                case 1:  pcszTmp = "SRC"; break;
                default: pcszTmp = "reserved";
            }
            pHlp->pfnPrintf(pHlp, "RA%02d: %s %RTmac\n", i, pcszTmp, ra->addr);
        }
    }


#ifdef E1K_INT_STATS
    pHlp->pfnPrintf(pHlp, "Interrupt attempts: %d\n", pState->uStatIntTry);
    pHlp->pfnPrintf(pHlp, "Interrupts raised : %d\n", pState->uStatInt);
    pHlp->pfnPrintf(pHlp, "Interrupts lowered: %d\n", pState->uStatIntLower);
    pHlp->pfnPrintf(pHlp, "Interrupts delayed: %d\n", pState->uStatIntDly);
    pHlp->pfnPrintf(pHlp, "Disabled delayed:   %d\n", pState->uStatDisDly);
    pHlp->pfnPrintf(pHlp, "Interrupts skipped: %d\n", pState->uStatIntSkip);
    pHlp->pfnPrintf(pHlp, "Masked interrupts : %d\n", pState->uStatIntMasked);
    pHlp->pfnPrintf(pHlp, "Early interrupts  : %d\n", pState->uStatIntEarly);
    pHlp->pfnPrintf(pHlp, "Late interrupts   : %d\n", pState->uStatIntLate);
    pHlp->pfnPrintf(pHlp, "Lost interrupts   : %d\n", pState->iStatIntLost);
    pHlp->pfnPrintf(pHlp, "Interrupts by RX  : %d\n", pState->uStatIntRx);
    pHlp->pfnPrintf(pHlp, "Interrupts by TX  : %d\n", pState->uStatIntTx);
    pHlp->pfnPrintf(pHlp, "Interrupts by ICS : %d\n", pState->uStatIntICS);
    pHlp->pfnPrintf(pHlp, "Interrupts by RDTR: %d\n", pState->uStatIntRDTR);
    pHlp->pfnPrintf(pHlp, "Interrupts by RDMT: %d\n", pState->uStatIntRXDMT0);
    pHlp->pfnPrintf(pHlp, "Interrupts by TXQE: %d\n", pState->uStatIntTXQE);
    pHlp->pfnPrintf(pHlp, "TX int delay asked: %d\n", pState->uStatTxIDE);
    pHlp->pfnPrintf(pHlp, "TX no report asked: %d\n", pState->uStatTxNoRS);
    pHlp->pfnPrintf(pHlp, "TX abs timer expd : %d\n", pState->uStatTAD);
    pHlp->pfnPrintf(pHlp, "TX int timer expd : %d\n", pState->uStatTID);
    pHlp->pfnPrintf(pHlp, "RX abs timer expd : %d\n", pState->uStatRAD);
    pHlp->pfnPrintf(pHlp, "RX int timer expd : %d\n", pState->uStatRID);
    pHlp->pfnPrintf(pHlp, "TX CTX descriptors: %d\n", pState->uStatDescCtx);
    pHlp->pfnPrintf(pHlp, "TX DAT descriptors: %d\n", pState->uStatDescDat);
    pHlp->pfnPrintf(pHlp, "TX LEG descriptors: %d\n", pState->uStatDescLeg);
    pHlp->pfnPrintf(pHlp, "Received frames   : %d\n", pState->uStatRxFrm);
    pHlp->pfnPrintf(pHlp, "Transmitted frames: %d\n", pState->uStatTxFrm);
#endif /* E1K_INT_STATS */

    e1kCsLeave(pState);
}

/**
 * Sets 8-bit register in PCI configuration space.
 * @param   refPciDev   The PCI device.
 * @param   uOffset     The register offset.
 * @param   u16Value    The value to store in the register.
 * @thread  EMT
 */
DECLINLINE(void) e1kPCICfgSetU8(PCIDEVICE& refPciDev, uint32_t uOffset, uint8_t u8Value)
{
    Assert(uOffset < sizeof(refPciDev.config));
    refPciDev.config[uOffset] = u8Value;
}

/**
 * Sets 16-bit register in PCI configuration space.
 * @param   refPciDev   The PCI device.
 * @param   uOffset     The register offset.
 * @param   u16Value    The value to store in the register.
 * @thread  EMT
 */
DECLINLINE(void) e1kPCICfgSetU16(PCIDEVICE& refPciDev, uint32_t uOffset, uint16_t u16Value)
{
    Assert(uOffset+sizeof(u16Value) <= sizeof(refPciDev.config));
    *(uint16_t*)&refPciDev.config[uOffset] = u16Value;
}

/**
 * Sets 32-bit register in PCI configuration space.
 * @param   refPciDev   The PCI device.
 * @param   uOffset     The register offset.
 * @param   u32Value    The value to store in the register.
 * @thread  EMT
 */
DECLINLINE(void) e1kPCICfgSetU32(PCIDEVICE& refPciDev, uint32_t uOffset, uint32_t u32Value)
{
    Assert(uOffset+sizeof(u32Value) <= sizeof(refPciDev.config));
    *(uint32_t*)&refPciDev.config[uOffset] = u32Value;
}

/**
 * Set PCI configuration space registers.
 *
 * @param   pci         Reference to PCI device structure.
 * @thread  EMT
 */
static DECLCALLBACK(void) e1kConfigurePCI(PCIDEVICE& pci, E1KCHIP eChip)
{
    Assert(eChip < RT_ELEMENTS(g_Chips));
    /* Configure PCI Device, assume 32-bit mode ******************************/
    PCIDevSetVendorId(&pci, g_Chips[eChip].uPCIVendorId);
    PCIDevSetDeviceId(&pci, g_Chips[eChip].uPCIDeviceId);
    e1kPCICfgSetU16(pci, VBOX_PCI_SUBSYSTEM_VENDOR_ID, g_Chips[eChip].uPCISubsystemVendorId);
    e1kPCICfgSetU16(pci, VBOX_PCI_SUBSYSTEM_ID, g_Chips[eChip].uPCISubsystemId);

    e1kPCICfgSetU16(pci, VBOX_PCI_COMMAND,            0x0000);
    /* DEVSEL Timing (medium device), 66 MHz Capable, New capabilities */
    e1kPCICfgSetU16(pci, VBOX_PCI_STATUS,
                    VBOX_PCI_STATUS_DEVSEL_MEDIUM | VBOX_PCI_STATUS_CAP_LIST |  VBOX_PCI_STATUS_66MHZ);
    /* Stepping A2 */
    e1kPCICfgSetU8( pci, VBOX_PCI_REVISION_ID,          0x02);
    /* Ethernet adapter */
    e1kPCICfgSetU8( pci, VBOX_PCI_CLASS_PROG,           0x00);
    e1kPCICfgSetU16(pci, VBOX_PCI_CLASS_DEVICE,       0x0200);
    /* normal single function Ethernet controller */
    e1kPCICfgSetU8( pci, VBOX_PCI_HEADER_TYPE,          0x00);
    /* Memory Register Base Address */
    e1kPCICfgSetU32(pci, VBOX_PCI_BASE_ADDRESS_0, 0x00000000);
    /* Memory Flash Base Address */
    e1kPCICfgSetU32(pci, VBOX_PCI_BASE_ADDRESS_1, 0x00000000);
    /* IO Register Base Address */
    e1kPCICfgSetU32(pci, VBOX_PCI_BASE_ADDRESS_2, 0x00000001);
    /* Expansion ROM Base Address */
    e1kPCICfgSetU32(pci, VBOX_PCI_ROM_ADDRESS,    0x00000000);
    /* Capabilities Pointer */
    e1kPCICfgSetU8( pci, VBOX_PCI_CAPABILITY_LIST,      0xDC);
    /* Interrupt Pin: INTA# */
    e1kPCICfgSetU8( pci, VBOX_PCI_INTERRUPT_PIN,        0x01);
    /* Max_Lat/Min_Gnt: very high priority and time slice */
    e1kPCICfgSetU8( pci, VBOX_PCI_MIN_GNT,              0xFF);
    e1kPCICfgSetU8( pci, VBOX_PCI_MAX_LAT,              0x00);

    /* PCI Power Management Registers ****************************************/
    /* Capability ID: PCI Power Management Registers */
    e1kPCICfgSetU8( pci, 0xDC,                           VBOX_PCI_CAP_ID_PM);
    /* Next Item Pointer: PCI-X */
    e1kPCICfgSetU8( pci, 0xDC + 1,                      0xE4);
    /* Power Management Capabilities: PM disabled, DSI */
    e1kPCICfgSetU16(pci, 0xDC + 2,
                    0x0002 | VBOX_PCI_PM_CAP_DSI);
    /* Power Management Control / Status Register: PM disabled */
    e1kPCICfgSetU16(pci, 0xDC + 4,                    0x0000);
    /* PMCSR_BSE Bridge Support Extensions: Not supported */
    e1kPCICfgSetU8( pci, 0xDC + 6,                      0x00);
    /* Data Register: PM disabled, always 0 */
    e1kPCICfgSetU8( pci, 0xDC + 7,                      0x00);

    /* PCI-X Configuration Registers *****************************************/
    /* Capability ID: PCI-X Configuration Registers */
    e1kPCICfgSetU8( pci, 0xE4,                           VBOX_PCI_CAP_ID_PCIX);
#ifdef E1K_WITH_MSI
    e1kPCICfgSetU8( pci, 0xE4 + 1,                      0x80);
#else
    /* Next Item Pointer: None (Message Signalled Interrupts are disabled) */
    e1kPCICfgSetU8( pci, 0xE4 + 1,                      0x00);
#endif
    /* PCI-X Command: Enable Relaxed Ordering */
    e1kPCICfgSetU16(pci, 0xE4 + 2,                    VBOX_PCI_X_CMD_ERO);
    /* PCI-X Status: 32-bit, 66MHz*/
    /// @todo: is this value really correct? fff8 doesn't look like actual PCI address
    e1kPCICfgSetU32(pci, 0xE4 + 4,                0x0040FFF8);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) e1kConstruct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    E1KSTATE* pState = PDMINS_2_DATA(pDevIns, E1KSTATE*);
    int       rc;
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);

    /* Init handles and log related stuff. */
    RTStrPrintf(pState->szInstance, sizeof(pState->szInstance), "E1000#%d", iInstance);
    E1kLog(("%s Constructing new instance sizeof(E1KRXDESC)=%d\n", INSTANCE(pState), sizeof(E1KRXDESC)));
    pState->hEventMoreRxDescAvail = NIL_RTSEMEVENT;

    /*
     * Validate configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg, "MAC\0" "CableConnected\0" "AdapterType\0" "LineSpeed\0" "GCEnabled\0" "R0Enabled\0"))
        return PDMDEV_SET_ERROR(pDevIns, VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES,
                                N_("Invalid configuration for E1000 device"));

    /** @todo: LineSpeed unused! */

    pState->fR0Enabled   = true;
    pState->fGCEnabled   = true;

    /* Get config params */
    rc = CFGMR3QueryBytes(pCfg, "MAC", pState->macConfigured.au8,
                          sizeof(pState->macConfigured.au8));
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to get MAC address"));
    rc = CFGMR3QueryBool(pCfg, "CableConnected", &pState->fCableConnected);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to get the value of 'CableConnected'"));
    rc = CFGMR3QueryU32(pCfg, "AdapterType", (uint32_t*)&pState->eChip);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to get the value of 'AdapterType'"));
    Assert(pState->eChip <= E1K_CHIP_82545EM);
    rc = CFGMR3QueryBoolDef(pCfg, "GCEnabled", &pState->fGCEnabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to get the value of 'GCEnabled'"));

    rc = CFGMR3QueryBoolDef(pCfg, "R0Enabled", &pState->fR0Enabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to get the value of 'R0Enabled'"));

    E1kLog(("%s Chip=%s\n", INSTANCE(pState), g_Chips[pState->eChip].pcszName));

    /* Initialize state structure */
    pState->pDevInsR3    = pDevIns;
    pState->pDevInsR0    = PDMDEVINS_2_R0PTR(pDevIns);
    pState->pDevInsRC    = PDMDEVINS_2_RCPTR(pDevIns);
    pState->u16TxPktLen  = 0;
    pState->fIPcsum      = false;
    pState->fTCPcsum     = false;
    pState->fIntMaskUsed = false;
    pState->fDelayInts   = false;
    pState->fLocked      = false;
    pState->u64AckedAt   = 0;
    pState->led.u32Magic = PDMLED_MAGIC;
    pState->u32PktNo     = 1;

#ifdef E1K_INT_STATS
    pState->uStatInt = 0;
    pState->uStatIntTry = 0;
    pState->uStatIntLower = 0;
    pState->uStatIntDly = 0;
    pState->uStatDisDly = 0;
    pState->iStatIntLost = 0;
    pState->iStatIntLostOne = 0;
    pState->uStatIntLate = 0;
    pState->uStatIntMasked = 0;
    pState->uStatIntEarly = 0;
    pState->uStatIntRx = 0;
    pState->uStatIntTx = 0;
    pState->uStatIntICS = 0;
    pState->uStatIntRDTR = 0;
    pState->uStatIntRXDMT0 = 0;
    pState->uStatIntTXQE = 0;
    pState->uStatTxNoRS = 0;
    pState->uStatTxIDE = 0;
    pState->uStatTAD = 0;
    pState->uStatTID = 0;
    pState->uStatRAD = 0;
    pState->uStatRID = 0;
    pState->uStatRxFrm = 0;
    pState->uStatTxFrm = 0;
    pState->uStatDescCtx = 0;
    pState->uStatDescDat = 0;
    pState->uStatDescLeg = 0;
#endif /* E1K_INT_STATS */

    /* Interfaces */
    pState->IBase.pfnQueryInterface          = e1kQueryInterface;

    pState->INetworkDown.pfnWaitReceiveAvail = e1kNetworkDown_WaitReceiveAvail;
    pState->INetworkDown.pfnReceive          = e1kNetworkDown_Receive;
    pState->INetworkDown.pfnXmitPending      = e1kNetworkDown_XmitPending;

    pState->ILeds.pfnQueryStatusLed          = e1kQueryStatusLed;

    pState->INetworkConfig.pfnGetMac         = e1kGetMac;
    pState->INetworkConfig.pfnGetLinkState   = e1kGetLinkState;
    pState->INetworkConfig.pfnSetLinkState   = e1kSetLinkState;

    /* Initialize the EEPROM */
    pState->eeprom.init(pState->macConfigured);

    /* Initialize internal PHY */
    Phy::init(&pState->phy, iInstance,
              pState->eChip == E1K_CHIP_82543GC?
              PHY_EPID_M881000 : PHY_EPID_M881011);
    Phy::setLinkStatus(&pState->phy, pState->fCableConnected);

    rc = PDMDevHlpSSMRegisterEx(pDevIns, E1K_SAVEDSTATE_VERSION, sizeof(E1KSTATE), NULL,
                                NULL,        e1kLiveExec, NULL,
                                e1kSavePrep, e1kSaveExec, NULL,
                                e1kLoadPrep, e1kLoadExec, e1kLoadDone);
    if (RT_FAILURE(rc))
        return rc;

    /* Initialize critical section */
    rc = PDMDevHlpCritSectInit(pDevIns, &pState->cs, RT_SRC_POS, "%s", pState->szInstance);
    if (RT_FAILURE(rc))
        return rc;
#ifndef E1K_GLOBAL_MUTEX
    rc = PDMDevHlpCritSectInit(pDevIns, &pState->csRx, RT_SRC_POS, "%sRX", pState->szInstance);
    if (RT_FAILURE(rc))
        return rc;
#endif

    /* Set PCI config registers */
    e1kConfigurePCI(pState->pciDevice, pState->eChip);
    /* Register PCI device */
    rc = PDMDevHlpPCIRegister(pDevIns, &pState->pciDevice);
    if (RT_FAILURE(rc))
        return rc;

#ifdef E1K_WITH_MSI
    PDMMSIREG aMsiReg;
    aMsiReg.cVectors = 1;
    aMsiReg.iCapOffset = 0x80;
    aMsiReg.iNextOffset = 0x0;
    aMsiReg.iMsiFlags = 0;
    rc = PDMDevHlpPCIRegisterMsi(pDevIns, &aMsiReg);
    AssertRC(rc);
    if (RT_FAILURE (rc))
        return rc;
#endif


    /* Map our registers to memory space (region 0, see e1kConfigurePCI)*/
    rc = PDMDevHlpPCIIORegionRegister(pDevIns, 0, E1K_MM_SIZE,
                                      PCI_ADDRESS_SPACE_MEM, e1kMap);
    if (RT_FAILURE(rc))
        return rc;
    /* Map our registers to IO space (region 2, see e1kConfigurePCI) */
    rc = PDMDevHlpPCIIORegionRegister(pDevIns, 2, E1K_IOPORT_SIZE,
                                      PCI_ADDRESS_SPACE_IO, e1kMap);
    if (RT_FAILURE(rc))
        return rc;

    /* Create transmit queue */
    rc = PDMDevHlpQueueCreate(pDevIns, sizeof(PDMQUEUEITEMCORE), 1, 0,
                              e1kTxQueueConsumer, true, "E1000-Xmit", &pState->pTxQueueR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pTxQueueR0 = PDMQueueR0Ptr(pState->pTxQueueR3);
    pState->pTxQueueRC = PDMQueueRCPtr(pState->pTxQueueR3);

    /* Create the RX notifier signaller. */
    rc = PDMDevHlpQueueCreate(pDevIns, sizeof(PDMQUEUEITEMCORE), 1, 0,
                              e1kCanRxQueueConsumer, true, "E1000-Rcv", &pState->pCanRxQueueR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pCanRxQueueR0 = PDMQueueR0Ptr(pState->pCanRxQueueR3);
    pState->pCanRxQueueRC = PDMQueueRCPtr(pState->pCanRxQueueR3);

#ifdef E1K_USE_TX_TIMERS
    /* Create Transmit Interrupt Delay Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kTxIntDelayTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Transmit Interrupt Delay Timer", &pState->pTIDTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pTIDTimerR0 = TMTimerR0Ptr(pState->pTIDTimerR3);
    pState->pTIDTimerRC = TMTimerRCPtr(pState->pTIDTimerR3);

# ifndef E1K_NO_TAD
    /* Create Transmit Absolute Delay Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kTxAbsDelayTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Transmit Absolute Delay Timer", &pState->pTADTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pTADTimerR0 = TMTimerR0Ptr(pState->pTADTimerR3);
    pState->pTADTimerRC = TMTimerRCPtr(pState->pTADTimerR3);
# endif /* E1K_NO_TAD */
#endif /* E1K_USE_TX_TIMERS */

#ifdef E1K_USE_RX_TIMERS
    /* Create Receive Interrupt Delay Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kRxIntDelayTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Receive Interrupt Delay Timer", &pState->pRIDTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pRIDTimerR0 = TMTimerR0Ptr(pState->pRIDTimerR3);
    pState->pRIDTimerRC = TMTimerRCPtr(pState->pRIDTimerR3);

    /* Create Receive Absolute Delay Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kRxAbsDelayTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Receive Absolute Delay Timer", &pState->pRADTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pRADTimerR0 = TMTimerR0Ptr(pState->pRADTimerR3);
    pState->pRADTimerRC = TMTimerRCPtr(pState->pRADTimerR3);
#endif /* E1K_USE_RX_TIMERS */

    /* Create Late Interrupt Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kLateIntTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Late Interrupt Timer", &pState->pIntTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pIntTimerR0 = TMTimerR0Ptr(pState->pIntTimerR3);
    pState->pIntTimerRC = TMTimerRCPtr(pState->pIntTimerR3);

    /* Create Link Up Timer */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, e1kLinkUpTimer, pState,
                                TMTIMER_FLAGS_NO_CRIT_SECT,
                                "E1000 Link Up Timer", &pState->pLUTimerR3);
    if (RT_FAILURE(rc))
        return rc;
    pState->pLUTimerR0 = TMTimerR0Ptr(pState->pLUTimerR3);
    pState->pLUTimerRC = TMTimerRCPtr(pState->pLUTimerR3);

    /* Register the info item */
    char szTmp[20];
    RTStrPrintf(szTmp, sizeof(szTmp), "e1k%d", iInstance);
    PDMDevHlpDBGFInfoRegister(pDevIns, szTmp, "E1000 info.", e1kInfo);

    /* Status driver */
    PPDMIBASE pBase;
    rc = PDMDevHlpDriverAttach(pDevIns, PDM_STATUS_LUN, &pState->IBase, &pBase, "Status Port");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to attach the status LUN"));
    pState->pLedsConnector = PDMIBASE_QUERY_INTERFACE(pBase, PDMILEDCONNECTORS);

    rc = PDMDevHlpDriverAttach(pDevIns, 0, &pState->IBase, &pState->pDrvBase, "Network Port");
    if (RT_SUCCESS(rc))
    {
        if (rc == VINF_NAT_DNS)
        {
            PDMDevHlpVMSetRuntimeError(pDevIns, 0 /*fFlags*/, "NoDNSforNAT",
                                       N_("A Domain Name Server (DNS) for NAT networking could not be determined. Ensure that your host is correctly connected to an ISP. If you ignore this warning the guest will not be able to perform nameserver lookups and it will probably observe delays if trying so"));
        }
        pState->pDrvR3 = PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMINETWORKUP);
        AssertMsgReturn(pState->pDrvR3, ("Failed to obtain the PDMINETWORKUP interface!\n"),
                        VERR_PDM_MISSING_INTERFACE_BELOW);

        pState->pDrvR0 = PDMIBASER0_QUERY_INTERFACE(PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMIBASER0), PDMINETWORKUP);
        pState->pDrvRC = PDMIBASERC_QUERY_INTERFACE(PDMIBASE_QUERY_INTERFACE(pState->pDrvBase, PDMIBASERC), PDMINETWORKUP);
    }
    else if (   rc == VERR_PDM_NO_ATTACHED_DRIVER
             || rc == VERR_PDM_CFG_MISSING_DRIVER_NAME)
    {
        /* No error! */
        E1kLog(("%s This adapter is not attached to any network!\n", INSTANCE(pState)));
    }
    else
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to attach the network LUN"));

    rc = RTSemEventCreate(&pState->hEventMoreRxDescAvail);
    if (RT_FAILURE(rc))
        return rc;

    e1kHardReset(pState);

#if defined(VBOX_WITH_STATISTICS) || defined(E1K_REL_STATS)
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatMMIOReadRZ,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling MMIO reads in RZ",         "/Devices/E1k%d/MMIO/ReadRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatMMIOReadR3,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling MMIO reads in R3",         "/Devices/E1k%d/MMIO/ReadR3", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatMMIOWriteRZ,        STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling MMIO writes in RZ",        "/Devices/E1k%d/MMIO/WriteRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatMMIOWriteR3,        STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling MMIO writes in R3",        "/Devices/E1k%d/MMIO/WriteR3", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatEEPROMRead,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling EEPROM reads",             "/Devices/E1k%d/EEPROM/Read", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatEEPROMWrite,        STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling EEPROM writes",            "/Devices/E1k%d/EEPROM/Write", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIOReadRZ,           STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling IO reads in RZ",           "/Devices/E1k%d/IO/ReadRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIOReadR3,           STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling IO reads in R3",           "/Devices/E1k%d/IO/ReadR3", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIOWriteRZ,          STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling IO writes in RZ",          "/Devices/E1k%d/IO/WriteRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIOWriteR3,          STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling IO writes in R3",          "/Devices/E1k%d/IO/WriteR3", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatLateIntTimer,       STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling late int timer",           "/Devices/E1k%d/LateInt/Timer", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatLateInts,           STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of late interrupts",          "/Devices/E1k%d/LateInt/Occured", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIntsRaised,         STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of raised interrupts",        "/Devices/E1k%d/Interrupts/Raised", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatIntsPrevented,      STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of prevented interrupts",     "/Devices/E1k%d/Interrupts/Prevented", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatReceive,            STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling receive",                  "/Devices/E1k%d/Receive/Total", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatReceiveFilter,      STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling receive filtering",        "/Devices/E1k%d/Receive/Filter", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatReceiveStore,       STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling receive storing",          "/Devices/E1k%d/Receive/Store", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatRxOverflow,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_OCCURENCE, "Profiling RX overflows",        "/Devices/E1k%d/RxOverflow", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatRxOverflowWakeup,   STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Nr of RX overflow wakeups",          "/Devices/E1k%d/RxOverflowWakeup", iInstance);
#endif /* VBOX_WITH_STATISTICS || E1K_REL_STATS */
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatReceiveBytes,       STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_BYTES,          "Amount of data received",            "/Devices/E1k%d/ReceiveBytes", iInstance);
#if defined(VBOX_WITH_STATISTICS) || defined(E1K_REL_STATS)
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTransmitRZ,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling transmits in RZ",          "/Devices/E1k%d/Transmit/TotalRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTransmitR3,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling transmits in R3",          "/Devices/E1k%d/Transmit/TotalR3", iInstance);
#endif /* VBOX_WITH_STATISTICS || E1K_REL_STATS */
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTransmitBytes,      STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_BYTES,          "Amount of data transmitted",         "/Devices/E1k%d/TransmitBytes", iInstance);
#if defined(VBOX_WITH_STATISTICS) || defined(E1K_REL_STATS)
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTransmitSendRZ,     STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling send transmit in RZ",      "/Devices/E1k%d/Transmit/SendRZ", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTransmitSendR3,     STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_TICKS_PER_CALL, "Profiling send transmit in R3",      "/Devices/E1k%d/Transmit/SendR3", iInstance);

    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxDescCtxNormal,    STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of normal context descriptors","/Devices/E1k%d/TxDesc/ContexNormal", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxDescCtxTSE,       STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of TSE context descriptors",  "/Devices/E1k%d/TxDesc/ContextTSE", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxDescData,         STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of TX data descriptors",      "/Devices/E1k%d/TxDesc/Data", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxDescLegacy,       STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of TX legacy descriptors",    "/Devices/E1k%d/TxDesc/Legacy", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxDescTSEData,      STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of TX TSE data descriptors",  "/Devices/E1k%d/TxDesc/TSEData", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxPathFallback,     STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Fallback TSE descriptor path",       "/Devices/E1k%d/TxPath/Fallback", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxPathGSO,          STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "GSO TSE descriptor path",            "/Devices/E1k%d/TxPath/GSO", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatTxPathRegular,      STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Regular descriptor path",            "/Devices/E1k%d/TxPath/Normal", iInstance);
    PDMDevHlpSTAMRegisterF(pDevIns, &pState->StatPHYAccesses,        STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,     "Number of PHY accesses",             "/Devices/E1k%d/PHYAccesses", iInstance);
#endif /* VBOX_WITH_STATISTICS || E1K_REL_STATS */

    return VINF_SUCCESS;
}

/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceE1000 =
{
    /* Structure version. PDM_DEVREG_VERSION defines the current version. */
    PDM_DEVREG_VERSION,
    /* Device name. */
    "e1000",
    /* Name of guest context module (no path).
     * Only evalutated if PDM_DEVREG_FLAGS_RC is set. */
    "VBoxDDGC.gc",
    /* Name of ring-0 module (no path).
     * Only evalutated if PDM_DEVREG_FLAGS_RC is set. */
    "VBoxDDR0.r0",
    /* The description of the device. The UTF-8 string pointed to shall, like this structure,
     * remain unchanged from registration till VM destruction. */
    "Intel PRO/1000 MT Desktop Ethernet.\n",

    /* Flags, combination of the PDM_DEVREG_FLAGS_* \#defines. */
    PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0,
    /* Device class(es), combination of the PDM_DEVREG_CLASS_* \#defines. */
    PDM_DEVREG_CLASS_NETWORK,
    /* Maximum number of instances (per VM). */
    8,
    /* Size of the instance data. */
    sizeof(E1KSTATE),

    /* Construct instance - required. */
    e1kConstruct,
    /* Destruct instance - optional. */
    e1kDestruct,
    /* Relocation command - optional. */
    e1kRelocate,
    /* I/O Control interface - optional. */
    NULL,
    /* Power on notification - optional. */
    NULL,
    /* Reset notification - optional. */
    e1kReset,
    /* Suspend notification  - optional. */
    e1kSuspend,
    /* Resume notification - optional. */
    NULL,
    /* Attach command - optional. */
    e1kAttach,
    /* Detach notification - optional. */
    e1kDetach,
    /* Query a LUN base interface - optional. */
    NULL,
    /* Init complete notification - optional. */
    NULL,
    /* Power off notification - optional. */
    e1kPowerOff,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DEVREG_VERSION
};

#endif /* IN_RING3 */
#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */