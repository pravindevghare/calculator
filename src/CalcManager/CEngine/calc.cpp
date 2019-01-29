// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

/**************************************************************************\
 *** SCICALC Scientific Calculator for Windows 3.00.12
 *** (c)1989 Microsoft Corporation.  All Rights Reserved.
 ***
 *** scimain.c
 ***
 *** Definitions of all globals, WinMain procedure
 ***
 *** Last modification
 ***    Fri  22-Nov-1996
 ***
 *** 22-Nov-1996
 *** Converted Calc from floating point to infinite precision.
 *** The new math engine is in ..\ratpak
 ***
 ***
 *** 05-Jan-1990
 *** Calc did not have a floating point exception signal handler. This
 *** would cause CALC to be forced to exit on a FP exception as that's
 *** the default.
 *** The signal handler is defined in SCIFUNC.C, in WinMain we hook the
 *** the signal.
\**************************************************************************/
#include "pch.h"
#include "Header Files/CalcEngine.h"

#include "CalculatorResource.h"

using namespace std;
using namespace CalcEngine;

/**************************************************************************/
/*** Global variable declarations and initializations                   ***/
/**************************************************************************/

static constexpr int DEFAULT_MAX_DIGITS = 32;
static constexpr int DEFAULT_PRECISION = 32;
static constexpr long DEFAULT_RADIX = 10;

static constexpr wchar_t DEFAULT_DEC_SEPARATOR = L'.';
static constexpr wchar_t DEFAULT_GRP_SEPARATOR = L',';
static constexpr wstring_view DEFAULT_GRP_STR = L"3;0";
static constexpr wstring_view DEFAULT_NUMBER_STR = L"0";

// Read strings for keys, errors, trig types, etc.
// These will be copied from the resources to local memory.  A larger
// than needed block is allocated first and then reallocated once we
// know how much is actually used.

array<wstring, CSTRINGSENGMAX> CCalcEngine::s_engineStrings;

void CCalcEngine::LoadEngineStrings(CalculationManager::IResourceProvider& resourceProvider)
{
    for (size_t i = 0; i < s_engineStrings.size(); i++)
    {
        s_engineStrings[i] = resourceProvider.GetCEngineString(g_sids[i]);
    }
}

//////////////////////////////////////////////////
//
// InitialOneTimeOnlyNumberSetup
//
//////////////////////////////////////////////////
void CCalcEngine::InitialOneTimeOnlySetup(CalculationManager::IResourceProvider& resourceProvider)
{
    LoadEngineStrings(resourceProvider);

    // we must now setup all the ratpak constants and our arrayed pointers
    // to these constants.
    ChangeBaseConstants(DEFAULT_RADIX, DEFAULT_MAX_DIGITS, DEFAULT_PRECISION);
}

//////////////////////////////////////////////////
//
// CCalcEngine::CCalcEngine
//
//////////////////////////////////////////////////
CCalcEngine::CCalcEngine(bool fPrecedence, bool fIntegerMode, CalculationManager::IResourceProvider* const pResourceProvider, __in_opt ICalcDisplay *pCalcDisplay, __in_opt shared_ptr<IHistoryDisplay> pHistoryDisplay) :
    m_HistoryCollector(pCalcDisplay, pHistoryDisplay, DEFAULT_DEC_SEPARATOR),
    m_resourceProvider(pResourceProvider),
    m_bSetCalcState(false),
    m_fPrecedence(fPrecedence),
    m_fIntegerMode(fIntegerMode),
    m_pCalcDisplay(pCalcDisplay),
    m_input(DEFAULT_DEC_SEPARATOR),
    m_nOpCode(0),
    m_nPrevOpCode(0),
    m_openParenCount(0),
    m_nPrecNum(0),
    m_nTempCom(0),
    m_nLastCom(0),
    m_parenVals{},
    m_precedenceVals{},
    m_bChangeOp(false),
    m_bRecord(false),
    m_bError(false),
    m_bInv(false),
    m_nFE(FMT_FLOAT),
    m_bNoPrevEqu(true),
    m_numwidth(QWORD_WIDTH),
    m_angletype(ANGLE_DEG),
    m_radix(DEFAULT_RADIX),
    m_precision(DEFAULT_PRECISION),
    m_cIntDigitsSav(DEFAULT_MAX_DIGITS),
    m_decGrouping(),
    m_groupSeparator(DEFAULT_GRP_SEPARATOR),
    m_numberString(DEFAULT_NUMBER_STR),
    m_nOp(),
    m_nPrecOp(),
    m_memoryValue{make_unique<Rational>()},
    m_holdVal{},
    m_currentVal{},
    m_lastVal{}
{
    InitChopNumbers();

    m_dwWordBitWidth = DwWordBitWidthFromeNumWidth(m_numwidth);

    PRAT maxTrig = longtorat(10L);
    PRAT hundred = longtorat(100L);
    powrat(&maxTrig, hundred, m_radix, m_precision);
    m_maxTrigonometricNum = Rational{ maxTrig };
    destroyrat(maxTrig);
    destroyrat(hundred);

    SetRadixTypeAndNumWidth(DEC_RADIX, m_numwidth);
    SettingsChanged();
    DisplayNum();
}

void CCalcEngine::InitChopNumbers()
{
    // these rat numbers are set only once and then never change regardless of
    // base or precision changes
    assert(m_chopNumbers.size() >= 4);
    m_chopNumbers[0] = Rational{ rat_qword };
    m_chopNumbers[1] = Rational{ rat_dword };
    m_chopNumbers[2] = Rational{ rat_word };
    m_chopNumbers[3] = Rational{ rat_byte };

    // initialize the max dec number you can support for each of the supported bit length
    // this is basically max num in that width  / 2 in integer
    assert(m_chopNumbers.size() == m_maxDecimalValueStrings.size());
    for (size_t i = 0; i < m_chopNumbers.size(); i++)
    {
        PRAT hno = m_chopNumbers[i].ToPRAT();

        divrat(&hno, rat_two, m_precision);
        intrat(&hno, m_radix, m_precision);

        m_maxDecimalValueStrings[i] = NumObjToString(hno, 10, FMT_FLOAT, m_precision);

        NumObjDestroy(&hno);
    }
}

// Gets the number in memory for UI to keep it persisted and set it again to a different instance
// of CCalcEngine. Otherwise it will get destructed with the CalcEngine
unique_ptr<Rational> CCalcEngine::PersistedMemObject()
{
    return move(m_memoryValue);
}

void CCalcEngine::PersistedMemObject(Rational const& memObject)
{
    m_memoryValue = make_unique<Rational>(memObject);
}

void CCalcEngine::SettingsChanged()
{
    wchar_t lastDec = m_decimalSeparator;
    wstring decStr = m_resourceProvider->GetCEngineString(L"sDecimal");
    m_decimalSeparator = decStr.empty() ? DEFAULT_DEC_SEPARATOR : decStr.at(0);
    // Until it can be removed, continue to set ratpak decimal here
    SetDecimalSeparator(m_decimalSeparator);

    wchar_t lastSep = m_groupSeparator;
    wstring sepStr = m_resourceProvider->GetCEngineString(L"sThousand");
    m_groupSeparator = sepStr.empty() ? DEFAULT_GRP_SEPARATOR : sepStr.at(0);

    auto lastDecGrouping = m_decGrouping;
    wstring grpStr = m_resourceProvider->GetCEngineString(L"sGrouping");
    m_decGrouping = DigitGroupingStringToGroupingVector(grpStr.empty() ? DEFAULT_GRP_STR : grpStr);

    bool numChanged = false;

    // if the grouping pattern or thousands symbol changed we need to refresh the display
    if (m_decGrouping != lastDecGrouping || m_groupSeparator != lastSep)
    {
        numChanged = true;
    }

    // if the decimal symbol has changed we always do the following things
    if (m_decimalSeparator != lastDec)
    {
        // Re-initialize member variables' decimal point.
        m_input.SetDecimalSymbol(m_decimalSeparator);
        m_HistoryCollector.SetDecimalSymbol(m_decimalSeparator);

        // put the new decimal symbol into the table used to draw the decimal key
        s_engineStrings[IDS_DECIMAL] = m_decimalSeparator;

        // we need to redraw to update the decimal point button
        numChanged = true;
    }

    if (numChanged)
    {
        DisplayNum();
    }
}

wchar_t CCalcEngine::DecimalSeparator() const
{
    return m_decimalSeparator;
}
