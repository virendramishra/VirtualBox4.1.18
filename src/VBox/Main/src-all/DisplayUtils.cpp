/* $Id: DisplayUtils.cpp $ */
/** @file
 * Implementation of IDisplay helpers.
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include <DisplayUtils.h>

#include <iprt/log.h>
#include <VBox/err.h>
#include <VBox/vmm/ssm.h>

int readSavedDisplayScreenshot(const Utf8Str &strStateFilePath, uint32_t u32Type, uint8_t **ppu8Data, uint32_t *pcbData, uint32_t *pu32Width, uint32_t *pu32Height)
{
    LogFlowFunc(("u32Type = %d [%s]\n", u32Type, strStateFilePath.c_str()));

    /* @todo cache read data */
    if (strStateFilePath.isEmpty())
    {
        /* No saved state data. */
        return VERR_NOT_SUPPORTED;
    }

    uint8_t *pu8Data = NULL;
    uint32_t cbData = 0;
    uint32_t u32Width = 0;
    uint32_t u32Height = 0;

    PSSMHANDLE pSSM;
    int vrc = SSMR3Open(strStateFilePath.c_str(), 0 /*fFlags*/, &pSSM);
    if (RT_SUCCESS(vrc))
    {
        uint32_t uVersion;
        vrc = SSMR3Seek(pSSM, "DisplayScreenshot", 1100 /*iInstance*/, &uVersion);
        if (RT_SUCCESS(vrc))
        {
            if (uVersion == sSSMDisplayScreenshotVer)
            {
                uint32_t cBlocks;
                vrc = SSMR3GetU32(pSSM, &cBlocks);
                AssertRCReturn(vrc, vrc);

                for (uint32_t i = 0; i < cBlocks; i++)
                {
                    uint32_t cbBlock;
                    vrc = SSMR3GetU32(pSSM, &cbBlock);
                    AssertRCBreak(vrc);

                    uint32_t typeOfBlock;
                    vrc = SSMR3GetU32(pSSM, &typeOfBlock);
                    AssertRCBreak(vrc);

                    LogFlowFunc(("[%d] type %d, size %d bytes\n", i, typeOfBlock, cbBlock));

                    if (typeOfBlock == u32Type)
                    {
                        if (cbBlock > 2 * sizeof(uint32_t))
                        {
                            cbData = cbBlock - 2 * sizeof(uint32_t);
                            pu8Data = (uint8_t *)RTMemAlloc(cbData);
                            if (pu8Data == NULL)
                            {
                                vrc = VERR_NO_MEMORY;
                                break;
                            }

                            vrc = SSMR3GetU32(pSSM, &u32Width);
                            AssertRCBreak(vrc);
                            vrc = SSMR3GetU32(pSSM, &u32Height);
                            AssertRCBreak(vrc);
                            vrc = SSMR3GetMem(pSSM, pu8Data, cbData);
                            AssertRCBreak(vrc);
                        }
                        else
                        {
                            /* No saved state data. */
                            vrc = VERR_NOT_SUPPORTED;
                        }

                        break;
                    }
                    else
                    {
                        /* displaySSMSaveScreenshot did not write any data, if
                         * cbBlock was == 2 * sizeof (uint32_t).
                         */
                        if (cbBlock > 2 * sizeof (uint32_t))
                        {
                            vrc = SSMR3Skip(pSSM, cbBlock);
                            AssertRCBreak(vrc);
                        }
                    }
                }
            }
            else
            {
                vrc = VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
            }
        }

        SSMR3Close(pSSM);
    }

    if (RT_SUCCESS(vrc))
    {
        if (u32Type == 0 && cbData % 4 != 0)
        {
            /* Bitmap is 32bpp, so data is invalid. */
            vrc = VERR_SSM_UNEXPECTED_DATA;
        }
    }

    if (RT_SUCCESS(vrc))
    {
        *ppu8Data = pu8Data;
        *pcbData = cbData;
        *pu32Width = u32Width;
        *pu32Height = u32Height;
        LogFlowFunc(("cbData %d, u32Width %d, u32Height %d\n", cbData, u32Width, u32Height));
    }

    LogFlowFunc(("vrc %Rrc\n", vrc));
    return vrc;
}

void freeSavedDisplayScreenshot(uint8_t *pu8Data)
{
    /* @todo not necessary when caching is implemented. */
    RTMemFree(pu8Data);
}

int readSavedGuestSize(const Utf8Str &strStateFilePath, uint32_t u32ScreenId, uint32_t *pu32Width, uint32_t *pu32Height)
{
    LogFlowFunc(("u32ScreenId = %d [%s]\n", u32ScreenId, strStateFilePath.c_str()));

    /* @todo cache read data */
    if (strStateFilePath.isEmpty())
    {
        /* No saved state data. */
        return VERR_NOT_SUPPORTED;
    }

    uint32_t u32Width = 0;
    uint32_t u32Height = 0;

    PSSMHANDLE pSSM;
    int vrc = SSMR3Open(strStateFilePath.c_str(), 0 /*fFlags*/, &pSSM);
    if (RT_SUCCESS(vrc))
    {
        uint32_t uVersion;
        vrc = SSMR3Seek(pSSM, "DisplayData", 0 /*iInstance*/, &uVersion);
        if (RT_SUCCESS(vrc))
        {
            /* Only the second version is supported. */
            if (   uVersion == sSSMDisplayVer2
                || uVersion == sSSMDisplayVer3)
            {
                uint32_t cMonitors;
                SSMR3GetU32(pSSM, &cMonitors);
                if (u32ScreenId > cMonitors)
                    vrc = -2;
                else
                {
                    /* Skip all previous monitors and the first 3 entries. */
                    SSMR3Skip(pSSM, u32ScreenId * 5 * sizeof(uint32_t) + 3 * sizeof(uint32_t));
                    SSMR3GetU32(pSSM, &u32Width);
                    SSMR3GetU32(pSSM, &u32Height);
                }
            }
        }

        SSMR3Close(pSSM);
    }

    if (RT_SUCCESS(vrc))
    {
        *pu32Width = u32Width;
        *pu32Height = u32Height;
    }

    LogFlowFunc(("vrc %Rrc\n", vrc));
    return vrc;
}

