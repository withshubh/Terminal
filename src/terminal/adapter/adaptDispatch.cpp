/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "adaptDispatch.hpp"
#include "conGetSet.hpp"
#include "../../types/inc/Viewport.hpp"

using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

// Routine Description:
// - No Operation helper. It's just here to make sure they're always all the same.
// Arguments:
// - <none>
// Return Value:
// - Always false to signify we didn't handle it.
bool NoOp() { return false; }

AdaptDispatch::AdaptDispatch(_Inout_ ConGetSet* const pConApi,
                             _Inout_ AdaptDefaults* const pDefaults,
                             _In_ const WORD wDefaultTextAttributes)
    : _pConApi(pConApi),
      _pDefaults(pDefaults),
      _wDefaultTextAttributes(wDefaultTextAttributes),
      _wBrightnessState(0),
      _fChangedBackground(false),
      _fChangedForeground(false),
      _fChangedMetaAttrs(false),
      _TermOutput()
{
    // The top-left corner in VT-speak is 1,1. Our internal array uses 0 indexes, but VT uses 1,1 for top left corner.
    _coordSavedCursor.X = 1;
    _coordSavedCursor.Y = 1;
    _srScrollMargins = {0}; // initially, there are no scroll margins.
    _fIsSetColumnsEnabled = false; // by default, DECSCPP is disabled.
    // TODO:10086990 - Create a setting to re-enable this.

}

void AdaptDispatch::Print(_In_ wchar_t const wchPrintable)
{
    _pDefaults->Print(_TermOutput.TranslateKey(wchPrintable));
}

void AdaptDispatch::PrintString(_In_reads_(cch) wchar_t* const rgwch, _In_ size_t const cch)
{
    if (_TermOutput.NeedToTranslate())
    {
        for (size_t i = 0; i < cch; i++)
        {
            rgwch[i] = _TermOutput.TranslateKey(rgwch[i]);
        }
    }

    _pDefaults->PrintString(rgwch, cch);
}

// Routine Description:
// - Generalizes cursor movement for up/down/left/right and next/previous line.
// Arguments:
// - dir - Specific direction to move
// - uiDistance - Magnitude of the move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_CursorMovement(_In_ CursorDirection const dir, _In_ unsigned int const uiDistance) const
{
    // First retrieve some information about the buffer
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        COORD coordCursor = csbiex.dwCursorPosition;

        // For next/previous line, we unconditionally need to move the X position to the left edge of the viewport.
        switch (dir)
        {
        case CursorDirection::NextLine:
        case CursorDirection::PrevLine:
            coordCursor.X = csbiex.srWindow.Left;
            break;
        }

        // Safely convert the UINT magnitude of the move we were given into a short (which is the size the console deals with)
        SHORT sDelta = 0;
        fSuccess = SUCCEEDED(UIntToShort(uiDistance, &sDelta));

        if (fSuccess)
        {
            // Prepare our variables for math. All operations are some variation on these two parameters
            SHORT* pcoordVal = nullptr; // The coordinate X or Y gets modified
            SHORT sBoundaryVal = 0; // There is a particular edge of the viewport that is our boundary condition as we approach it.

            // Up and Down modify the Y coordinate. Left and Right modify the X.
            switch (dir)
            {
            case CursorDirection::Up:
            case CursorDirection::Down:
            case CursorDirection::NextLine:
            case CursorDirection::PrevLine:
                pcoordVal = &coordCursor.Y;
                break;
            case CursorDirection::Left:
            case CursorDirection::Right:
                pcoordVal = &coordCursor.X;
                break;
            default:
                fSuccess = false;
                break;
            }

            // Moving upward is bounded by top, etc.
            switch (dir)
            {
            case CursorDirection::Up:
            case CursorDirection::PrevLine:
                sBoundaryVal = csbiex.srWindow.Top;
                break;
            case CursorDirection::Down:
            case CursorDirection::NextLine:
                sBoundaryVal = csbiex.srWindow.Bottom;
                break;
            case CursorDirection::Left:
                sBoundaryVal = csbiex.srWindow.Left;
                break;
            case CursorDirection::Right:
                sBoundaryVal = csbiex.srWindow.Right;
                break;
            default:
                fSuccess = false;
                break;
            }

            if (fSuccess)
            {
                // For up and left, we need to subtract the magnitude of the vector to get the new spot. Right/down = add.
                // Use safe short subtraction to prevent under/overflow.
                switch (dir)
                {
                case CursorDirection::Up:
                case CursorDirection::Left:
                case CursorDirection::PrevLine:
                    fSuccess = SUCCEEDED(ShortSub(*pcoordVal, sDelta, pcoordVal));
                    break;
                case CursorDirection::Down:
                case CursorDirection::Right:
                case CursorDirection::NextLine:
                    fSuccess = SUCCEEDED(ShortAdd(*pcoordVal, sDelta, pcoordVal));
                    break;
                }

                if (fSuccess)
                {
                    // Now apply the boundary condition. Up, Left can't be smaller than their boundary. Top, Right can't be larger.
                    switch (dir)
                    {
                    case CursorDirection::Up:
                    case CursorDirection::Left:
                    case CursorDirection::PrevLine:
                        *pcoordVal = max(*pcoordVal, sBoundaryVal);
                        break;
                    case CursorDirection::Down:
                    case CursorDirection::Right:
                    case CursorDirection::NextLine:
                        // For the bottom and right edges, the viewport value is stated to be one outside the rectangle.
                        *pcoordVal = min(*pcoordVal, sBoundaryVal - 1);
                        break;
                    default:
                        fSuccess = false;
                        break;
                    }

                    if (fSuccess)
                    {
                        // Finally, attempt to set the adjusted cursor position back into the console.
                        fSuccess = !!_pConApi->SetConsoleCursorPosition(coordCursor);
                    }
                }
            }
        }
    }

    return fSuccess;
}

// Routine Description:
// - CUP - Handles cursor upward movement by given distance
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorUp(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::Up, uiDistance);
}

// Routine Description:
// - CUD - Handles cursor downward movement by given distance
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorDown(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::Down, uiDistance);
}

// Routine Description:
// - CUF - Handles cursor forward movement by given distance
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorForward(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::Right, uiDistance);
}

// Routine Description:
// - CUB - Handles cursor backward movement by given distance
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorBackward(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::Left, uiDistance);
}

// Routine Description:
// - CNL - Handles cursor movement to the following line (or N lines down)
// - Moves to the beginning X/Column position of the line.
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorNextLine(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::NextLine, uiDistance);
}

// Routine Description:
// - CPL - Handles cursor movement to the previous line (or N lines up)
// - Moves to the beginning X/Column position of the line.
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorPrevLine(_In_ unsigned int const uiDistance)
{
    return _CursorMovement(CursorDirection::PrevLine, uiDistance);
}

// Routine Description:
// - Generalizes cursor movement to a specific coordinate position
// - If a parameter is left blank, we will maintain the existing position in that dimension.
// Arguments:
// - puiRow - Optional row to move to
// - puiColumn - Optional column to move to
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_CursorMovePosition(_In_opt_ const unsigned int* const puiRow, _In_opt_ const unsigned int* const puiCol) const
{
    bool fSuccess = true;

    // First retrieve some information about the buffer
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        // handle optional parameters. If not specified, keep same cursor position from what we just loaded.
        unsigned int uiRow = 0;
        unsigned int uiCol = 0;

        if (puiRow != nullptr)
        {
            if (*puiRow != 0)
            {
                uiRow = *puiRow - 1; // In VT, the origin is 1,1. For our array, it's 0,0. So subtract 1.
            }
            else
            {
                fSuccess = false; // The parser should never return 0 (0 maps to 1), so this is a failure condition.
            }
        }
        else
        {
            uiRow = csbiex.dwCursorPosition.Y - csbiex.srWindow.Top; // remember, in VT speak, this is relative to the viewport. not absolute.
        }

        if (puiCol != nullptr)
        {
            if (*puiCol != 0)
            {
                uiCol = *puiCol - 1; // In VT, the origin is 1,1. For our array, it's 0,0. So subtract 1.
            }
            else
            {
                fSuccess = false; // The parser should never return 0 (0 maps to 1), so this is a failure condition.
            }
        }
        else
        {
            uiCol = csbiex.dwCursorPosition.X - csbiex.srWindow.Left; // remember, in VT speak, this is relative to the viewport. not absolute.
        }

        if (fSuccess)
        {
            COORD coordCursor = csbiex.dwCursorPosition;

            // Safely convert the UINT positions we were given into shorts (which is the size the console deals with)
            fSuccess = SUCCEEDED(UIntToShort(uiRow, &coordCursor.Y)) && SUCCEEDED(UIntToShort(uiCol, &coordCursor.X));

            if (fSuccess)
            {
                // Set the line and column values as offsets from the viewport edge. Use safe math to prevent overflow.
                fSuccess = SUCCEEDED(ShortAdd(coordCursor.Y, csbiex.srWindow.Top, &coordCursor.Y)) &&
                    SUCCEEDED(ShortAdd(coordCursor.X, csbiex.srWindow.Left, &coordCursor.X));

                if (fSuccess)
                {
                    // Apply boundary tests to ensure the cursor isn't outside the viewport rectangle.
                    coordCursor.Y = max(min(coordCursor.Y, csbiex.srWindow.Bottom - 1), csbiex.srWindow.Top);
                    coordCursor.X = max(min(coordCursor.X, csbiex.srWindow.Right - 1), csbiex.srWindow.Left);

                    // Finally, attempt to set the adjusted cursor position back into the console.
                    fSuccess = !!_pConApi->SetConsoleCursorPosition(coordCursor);
                }
            }
        }
    }

    return fSuccess;
}

// Routine Description:
// - CHA - Moves the cursor to an exact X/Column position on the current line.
// Arguments:
// - uiColumn - Specific X/Column position to move to
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorHorizontalPositionAbsolute(_In_ unsigned int const uiColumn)
{
    return _CursorMovePosition(nullptr, &uiColumn);
}

// Routine Description:
// - VPA - Moves the cursor to an exact Y/row position on the current column.
// Arguments:
// - uiLine - Specific Y/Row position to move to
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::VerticalLinePositionAbsolute(_In_ unsigned int const uiLine)
{
    return _CursorMovePosition(&uiLine, nullptr);
}

// Routine Description:
// - CUP - Moves the cursor to an exact X/Column and Y/Row/Line coordinate position.
// Arguments:
// - uiLine - Specific Y/Row/Line position to move to
// - uiColumn - Specific X/Column position to move to
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorPosition(_In_ unsigned int const uiLine, _In_ unsigned int const uiColumn)
{
    return _CursorMovePosition(&uiLine, &uiColumn);
}

// Routine Description:
// - DECSC - Saves the current cursor position into a memory buffer.
// Arguments:
// - <none>
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorSavePosition()
{
    // First retrieve some information about the buffer
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        // The cursor is given to us by the API as relative to the whole buffer.
        // But in VT speak, the cursor should be relative to the current viewport. Adjust.
        COORD const coordCursor = csbiex.dwCursorPosition;

        SMALL_RECT const srViewport = csbiex.srWindow;

        // VT is also 1 based, not 0 based, so correct by 1.
        _coordSavedCursor.X = coordCursor.X - srViewport.Left + 1;
        _coordSavedCursor.Y = coordCursor.Y - srViewport.Top + 1;
    }

    return fSuccess;
}

// Routine Description:
// - DECRC - Restores a saved cursor position from the DECSC command back into the console state.
// - If no position was set, this defaults to the top left corner (see AdaptDispatch constructor.)
// Arguments:
// - <none>
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorRestorePosition()
{
    unsigned int const uiRow = _coordSavedCursor.Y;
    unsigned int const uiCol = _coordSavedCursor.X;

    return _CursorMovePosition(&uiRow, &uiCol);
}

// Routine Description:
// - DECTCEM - Sets the show/hide visibility status of the cursor.
// Arguments:
// - fIsVisible - Turns the cursor rendering on (TRUE) or off (FALSE).
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::CursorVisibility(_In_ bool const fIsVisible)
{
    // First retrieve the existing cursor visibility structure (since we don't want to change the cursor height.)
    CONSOLE_CURSOR_INFO cci = { 0 };
    bool fSuccess = !!_pConApi->GetConsoleCursorInfo(&cci);

    if (fSuccess)
    {
        // Change only the visibility flag to match what we're given.
        cci.bVisible = fIsVisible;

        // Save it back.
        fSuccess = !!_pConApi->SetConsoleCursorInfo(&cci);
    }

    return fSuccess;
}


// Routine Description:
// - This helper will do the work of performing an insert or delete character operation
// - Both operations are similar in that they cut text and move it left or right in the buffer, padding the leftover area with spaces.
// Arguments:
// - uiCount - The number of characters to insert
// - fIsInsert - TRUE if insert mode (cut and paste to the right, away from the cursor). FALSE if delete mode (cut and paste to the left, toward the cursor)
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_InsertDeleteHelper(_In_ unsigned int const uiCount, _In_ bool const fIsInsert) const
{
    // We'll be doing short math on the distance since all console APIs use shorts. So check that we can successfully convert the uint into a short first.
    SHORT sDistance;
    bool fSuccess = SUCCEEDED(UIntToShort(uiCount, &sDistance));

    if (fSuccess)
    {
        // get current cursor
        CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
        csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
        fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

        if (fSuccess)
        {
            // Rectangle to cut out of the existing buffer
            SMALL_RECT srScroll;
            srScroll.Left = csbiex.dwCursorPosition.X;
            srScroll.Right = csbiex.srWindow.Right;
            srScroll.Top = csbiex.dwCursorPosition.Y;
            srScroll.Bottom = srScroll.Top;

            // Paste coordinate for cut text above
            COORD coordDestination;
            coordDestination.Y = csbiex.dwCursorPosition.Y;
            coordDestination.X = csbiex.dwCursorPosition.X;

            // Fill character for remaining space left behind by "cut" operation (or for fill if we "cut" the entire line)
            CHAR_INFO ciFill;
            ciFill.Attributes = csbiex.wAttributes;
            ciFill.Char.UnicodeChar = L' ';

            if (fIsInsert)
            {
                // Insert makes space by moving characters out to the right. So move the destination of the cut/paste region.
                fSuccess = SUCCEEDED(ShortAdd(coordDestination.X, sDistance, &coordDestination.X));
            }
            else
            {
                // for delete, we need to add to the scroll region to move it off toward the right.
                fSuccess = SUCCEEDED(ShortAdd(srScroll.Left, sDistance, &srScroll.Left));
            }

            if (fSuccess)
            {
                if (srScroll.Left >= csbiex.srWindow.Right || coordDestination.X >= csbiex.srWindow.Right)
                {
                    DWORD const nLength = csbiex.srWindow.Right - csbiex.dwCursorPosition.X;
                    DWORD dwWritten = 0;

                    // if the select/scroll region is off screen to the right or the destination is off screen to the right, fill instead of scrolling.
                    fSuccess = !!_pConApi->FillConsoleOutputCharacterW(ciFill.Char.UnicodeChar, nLength, csbiex.dwCursorPosition, &dwWritten);

                    if (fSuccess)
                    {
                        dwWritten = 0;
                        fSuccess = !!_pConApi->FillConsoleOutputAttribute(ciFill.Attributes, nLength, csbiex.dwCursorPosition, &dwWritten);
                    }
                }
                else
                {
                    // clip inside the viewport.
                    fSuccess = !!_pConApi->ScrollConsoleScreenBufferW(&srScroll, &csbiex.srWindow, coordDestination, &ciFill);
                }
            }
        }
    }

    return fSuccess;
}

// Routine Description:
// ICH - Insert Character - Blank/default attribute characters will be inserted at the current cursor position.
//     - Each inserted character will push all text in the row to the right.
// Arguments:
// - uiCount - The number of characters to insert
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::InsertCharacter(_In_ unsigned int const uiCount)
{
    return _InsertDeleteHelper(uiCount, true);
}

// Routine Description:
// DCH - Delete Character - The character at the cursor position will be deleted. Blank/attribute characters will
//       be inserted from the right edge of the current line.
// Arguments:
// - uiCount - The number of characters to delete
// Return Value:
// - True if handled successfuly. False otherwise.
bool AdaptDispatch::DeleteCharacter(_In_ unsigned int const uiCount)
{
    return _InsertDeleteHelper(uiCount, false);
}
// Routine Description:
// - Internal helper to erase a specific number of characters in one particular line of the buffer.
//     Erased positions are replaced with spaces.
// Arguments:
// - coordStartPosition - The position to begin erasing at.
// - dwLength - the number of characters to erase.
// - wFillColor - The attributes to apply to the erased positions.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_EraseSingleLineDistanceHelper(_In_ COORD const coordStartPosition, _In_ DWORD const dwLength, _In_ WORD const wFillColor) const
{
    WCHAR const wchSpace = static_cast<WCHAR>(0x20); // space character. use 0x20 instead of literal space because we can't assume the compiler will always turn ' ' into 0x20.

    DWORD dwCharsWritten = 0;
    bool fSuccess = !!_pConApi->FillConsoleOutputCharacterW(wchSpace, dwLength, coordStartPosition, &dwCharsWritten);

    if (fSuccess)
    {
        fSuccess = !!_pConApi->FillConsoleOutputAttribute(wFillColor, dwLength, coordStartPosition, &dwCharsWritten);
    }

    return fSuccess;
}

bool AdaptDispatch::_EraseAreaHelper(_In_ COORD const coordStartPosition, _In_ COORD const coordLastPosition, _In_ WORD const wFillColor)
{
    WCHAR const wchSpace = static_cast<WCHAR>(0x20); // space character. use 0x20 instead of literal space because we can't assume the compiler will always turn ' ' into 0x20.

    DWORD dwCharsWritten = 0;
    assert(coordStartPosition.X < coordLastPosition.X);
    assert(coordStartPosition.Y < coordLastPosition.Y);
    bool fSuccess = false;
    for (short y = coordStartPosition.Y; y < coordLastPosition.Y; y++)
    {
        const COORD coordLine = {coordStartPosition.X, y};
        fSuccess = !!_pConApi->FillConsoleOutputCharacterW(wchSpace, coordLastPosition.X - coordStartPosition.X, coordLine, &dwCharsWritten);
        if (fSuccess)
        {
            fSuccess = !!_pConApi->FillConsoleOutputAttribute(wFillColor, coordLastPosition.X - coordStartPosition.X, coordLine, &dwCharsWritten);
        }

        if (!fSuccess)
        {
            break;
        }
    }
    return fSuccess;
}

// Routine Description:
// - Internal helper to erase one particular line of the buffer. Either from beginning to the cursor, from the cursor to the end, or the entire line.
// - Used by both erase line (used just once) and by erase screen (used in a loop) to erase a portion of the buffer.
// Arguments:
// - pcsbiex - Pointer to the console screen buffer that we will be erasing (and getting cursor data from within)
// - eraseType - Enumeration mode of which kind of erase to perform: beginning to cursor, cursor to end, or entire line.
// - sLineId - The line number (array index value, starts at 0) of the line to operate on within the buffer.
//           - This is not aware of circular buffer. Line 0 is always the top visible line if you scrolled the whole way up the window.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_EraseSingleLineHelper(_In_ const CONSOLE_SCREEN_BUFFER_INFOEX* const pcsbiex, _In_ EraseType const eraseType, _In_ SHORT const sLineId, _In_ WORD const wFillColor) const
{
    COORD coordStartPosition = { 0 };
    coordStartPosition.Y = sLineId ;

    // determine start position from the erase type
    // remember that erases are inclusive of the current cursor position.
    switch (eraseType)
    {
    case EraseType::FromBeginning:
    case EraseType::All:
        coordStartPosition.X = pcsbiex->srWindow.Left; // from beginning and the whole line start from the left viewport edge.
        break;
    case EraseType::ToEnd:
        coordStartPosition.X = pcsbiex->dwCursorPosition.X; // from the current cursor position (including it)
        break;
    }

    DWORD nLength = 0;

    // determine length of erase from erase type
    switch (eraseType)
    {
    case EraseType::FromBeginning:
        // +1 because if cursor were at the left edge, the length would be 0 and we want to paint at least the 1 character the cursor is on.
        nLength = (pcsbiex->dwCursorPosition.X - pcsbiex->srWindow.Left) + 1;
        break;
    case EraseType::ToEnd:
    case EraseType::All:
        // Remember the .Right value is 1 farther than the right most displayed character in the viewport. Therefore no +1.
        nLength = pcsbiex->srWindow.Right - coordStartPosition.X;
        break;
    }

    return _EraseSingleLineDistanceHelper(coordStartPosition, nLength, wFillColor);

}

// Routine Description:
// - ECH - Erase Characters from the current cursor position, by replacing
//     them with a space. This will only erase characters in the current line,
//     and won't wrap to the next. The attributes of any erased positions
//     recieve the currently selected attributes.
// Arguments:
// - uiNumChars - The number of characters to erase.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::EraseCharacters(_In_ unsigned int const uiNumChars)
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        const COORD coordStartPosition = csbiex.dwCursorPosition;

        const SHORT sRemainingSpaces = csbiex.srWindow.Right - coordStartPosition.X;
        const unsigned short usActualRemaining = (sRemainingSpaces < 0)? 0 : sRemainingSpaces;
        // erase at max the number of characters remaining in the line from the current position.
        const DWORD dwEraseLength = (uiNumChars <= usActualRemaining)? uiNumChars : usActualRemaining;

        fSuccess = _EraseSingleLineDistanceHelper(coordStartPosition, dwEraseLength, csbiex.wAttributes);
    }
    return fSuccess;
}

// Routine Description:
// - ED - Erases a portion of the current viewable area (viewport) of the console.
// Arguments:
// - eraseType - Determines whether to erase:
//      From beginning (top-left corner) to the cursor
//      From cursor to end (bottom-right corner)
//      The entire viewport area
//      The scrollback (outside the viewport area)
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::EraseInDisplay(_In_ EraseType const eraseType)
{
    // First things first. If this is a "Scrollback" clear, then just do that.
    // Scrollback clears erase everything in the "scrollback" of a *nix terminal
    //      Everything that's scrolled off the screen so far.
    // Or if it's an Erase All, then we also need to handle that specially
    //      by moving the current contents of the viewport into the scrollback.
    if (eraseType == EraseType::Scrollback)
    {
        return _EraseScrollback();
    }
    else if (eraseType == EraseType::All)
    {
        return _EraseAll();
    }

    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        // What we need to erase is grouped into 3 types:
        // 1. Lines before cursor
        // 2. Cursor Line
        // 3. Lines after cursor
        // We erase one or more of these based on the erase type:
        // A. FromBeginning - Erase 1 and Some of 2.
        // B. ToEnd - Erase some of 2 and 3.
        // C. All - Erase 1, 2, and 3.

        // 1. Lines before cursor line
        if (eraseType == EraseType::FromBeginning)
        {
            // For beginning and all, erase all complete lines before (above vertically) from the cursor position.
            for (SHORT sStartLine = csbiex.srWindow.Top; sStartLine < csbiex.dwCursorPosition.Y; sStartLine++)
            {
                fSuccess = _EraseSingleLineHelper(&csbiex, EraseType::All, sStartLine, csbiex.wAttributes);

                if (!fSuccess)
                {
                    break;
                }
            }
        }

        if (fSuccess)
        {
            // 2. Cursor Line
            fSuccess = _EraseSingleLineHelper(&csbiex, eraseType, csbiex.dwCursorPosition.Y, csbiex.wAttributes);
        }

        if (fSuccess)
        {
            // 3. Lines after cursor line
            if (eraseType == EraseType::ToEnd)
            {
                // For beginning and all, erase all complete lines after (below vertically) the cursor position.
                // Remember that the viewport bottom value is 1 beyond the viewable area of the viewport.
                for (SHORT sStartLine = csbiex.dwCursorPosition.Y + 1; sStartLine < csbiex.srWindow.Bottom; sStartLine++)
                {
                    fSuccess = _EraseSingleLineHelper(&csbiex, EraseType::All, sStartLine, csbiex.wAttributes);

                    if (!fSuccess)
                    {
                        break;
                    }
                }
            }
        }
    }

    return fSuccess;
}

// Routine Description:
// - EL - Erases the line that the cursor is currently on.
// Arguments:
// - eraseType - Determines whether to erase: From beginning (left edge) to the cursor, from cursor to end (right edge), or the entire line.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::EraseInLine(_In_ EraseType const eraseType)
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        fSuccess = _EraseSingleLineHelper(&csbiex, eraseType, csbiex.dwCursorPosition.Y, csbiex.wAttributes);
    }

    return fSuccess;
}


// Routine Description:
// - DSR - Reports status of a console property back to the STDIN based on the type of status requested.
//       - This particular routine responds to ANSI status patterns only (CSI # n), not the DEC format (CSI ? # n)
// Arguments:
// - statusType - ANSI status type indicating what property we should report back
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::DeviceStatusReport(_In_ AnsiStatusType const statusType)
{
    bool fSuccess = false;

    switch (statusType)
    {
    case AnsiStatusType::CPR_CursorPositionReport:
        fSuccess = _CursorPositionReport();
        break;
    }

    return fSuccess;
}

// Routine Description:
// - DA - Reports the identity of this Virtual Terminal machine to the caller.
//      - In our case, we'll report back to acknowledge we understand, but reveal no "hardware" upgrades like physical terminals of old.
// Arguments:
// - <none>
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::DeviceAttributes()
{
    // See: http://vt100.net/docs/vt100-ug/chapter3.html#DA
    wchar_t* const pwszResponse = L"\x1b[?1;0c";

    return _WriteResponse(pwszResponse, wcslen(pwszResponse));
}

// Routine Description:
// - DSR-CPR - Reports the current cursor position within the viewport back to the input channel
// Arguments:
// - <none>
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_CursorPositionReport() const
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    if (fSuccess)
    {
        // First pull the cursor position relative to the entire buffer out of the console.
        COORD coordCursorPos = csbiex.dwCursorPosition;

        // Now adjust it for its position in respect to the current viewport.
        coordCursorPos.X -= csbiex.srWindow.Left;
        coordCursorPos.Y -= csbiex.srWindow.Top;

        // NOTE: 1,1 is the top-left corner of the viewport in VT-speak, so add 1.
        coordCursorPos.X++;
        coordCursorPos.Y++;

        // Now send it back into the input channel of the console.
        // First format the response string.
        wchar_t pwszResponseBuffer[50];
        swprintf_s(pwszResponseBuffer, ARRAYSIZE(pwszResponseBuffer), L"\x1b[%d;%dR", coordCursorPos.Y, coordCursorPos.X);

        size_t const cBuffer = wcslen(pwszResponseBuffer);

        fSuccess = _WriteResponse(pwszResponseBuffer, cBuffer);
    }

    return fSuccess;
}

// Routine Description:
// - Helper to send a string reply to the input stream of the console.
// - Used by various commands where the program attached would like a reply to one of the commands issued.
// - This will generate two "key presses" (one down, one up) for every character in the string and place them into the head of the console's input stream.
// Arguments:
// - pwszReply - The reply string to transmit back to the input stream
// - cReply - The length of the string.
// Return Value:
// - True if the string was converted to input events and placed into the console input buffer successfuly. False otherwise.
bool AdaptDispatch::_WriteResponse(_In_reads_(cchReply) PCWSTR pwszReply, _In_ size_t const cchReply) const
{
    bool fSuccess = false;
    std::deque<std::unique_ptr<IInputEvent>> inEvents;
    try
    {
        // generate a paired key down and key up event for every
        // character to be sent into the console's input buffer
        for (size_t i = 0; i < cchReply; ++i)
        {
            // This wasn't from a real keyboard, so we're leaving key/scan codes blank.
            KeyEvent keyEvent{ TRUE, 1, 0, 0, pwszReply[i], 0 };

            inEvents.push_back(std::make_unique<KeyEvent>(keyEvent));
            keyEvent.SetKeyDown(false);
            inEvents.push_back(std::make_unique<KeyEvent>(keyEvent));
        }
    }
    catch (...)
    {
        LOG_HR(wil::ResultFromCaughtException());
        return false;
    }

    size_t eventsWritten;
    fSuccess = !!_pConApi->PrivatePrependConsoleInput(inEvents, eventsWritten);

    return fSuccess;
}

// Routine Description:
// - Generalizes scrolling movement for up/down
// Arguments:
// - sdDirection - Specific direction to move
// - uiDistance - Magnitude of the move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_ScrollMovement(_In_ ScrollDirection const sdDirection, _In_ unsigned int const uiDistance) const
{
    // We'll be doing short math on the distance since all console APIs use shorts. So check that we can successfully convert the uint into a short first.
    SHORT sDistance;
    bool fSuccess = SUCCEEDED(UIntToShort(uiDistance, &sDistance));

    if (fSuccess)
    {
        // get current cursor
        CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
        csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
        fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

        if (fSuccess)
        {
            SMALL_RECT srScreen = csbiex.srWindow;
            COORD Cursor = csbiex.dwCursorPosition;

            // Paste coordinate for cut text above
            COORD coordDestination;
            coordDestination.X = srScreen.Left;
            // Scroll starting from the top of the scroll margins.
            coordDestination.Y = (_srScrollMargins.Top + srScreen.Top) + sDistance * (sdDirection == ScrollDirection::Up? -1 : 1);
            // We don't need to worry about clipping the margins at all, ScrollRegion inside conhost will do that correctly for us

            // Fill character for remaining space left behind by "cut" operation (or for fill if we "cut" the entire line)
            CHAR_INFO ciFill;
            ciFill.Attributes = csbiex.wAttributes;
            ciFill.Char.UnicodeChar = L' ';
            fSuccess = !!_pConApi->ScrollConsoleScreenBufferW(&srScreen, &srScreen, coordDestination, &ciFill);
        }
    }

    return fSuccess;
}

// Routine Description:
// - SU - Pans the window DOWN by given distance (uiDistance new lines appear at the bottom of the screen)
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::ScrollUp(_In_ unsigned int const uiDistance)
{
    return _ScrollMovement(ScrollDirection::Up, uiDistance);
}

// Routine Description:
// - SD - Pans the window UP by given distance (uiDistance new lines appear at the top of the screen)
// Arguments:
// - uiDistance - Distance to move
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::ScrollDown(_In_ unsigned int const uiDistance)
{
    return _ScrollMovement(ScrollDirection::Down, uiDistance);
}

// Routine Description:
// - DECSCPP / DECCOLM Sets the number of columns "per page" AKA sets the console width.
// DECCOLM also clear the screen (like a CSI 2 J sequence), while DECSCPP just sets the width.
// (DECCOLM will do this seperately of this function)
// Arguments:
// - uiColumns - Number of columns
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetColumns(_In_ unsigned int const uiColumns)
{
    if (!_fIsSetColumnsEnabled)
    {
        // Only set columns if that option is available. Return true, as this is technically a successful handling.
        return true;
    }

    SHORT sColumns;
    bool fSuccess = SUCCEEDED(UIntToShort(uiColumns, &sColumns));
    if (fSuccess)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
        csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
        fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

        if (fSuccess)
        {
            csbiex.dwSize.X = sColumns;
            fSuccess = !!_pConApi->SetConsoleScreenBufferInfoEx(&csbiex);
        }
    }
    return fSuccess;
}

// Routine Description:
// - DECCOLM not only sets the number of columns, but also clears the screen buffer, resets the page margins, and places the cursor at 1,1
// Arguments:
// - uiColumns - Number of columns
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_DoDECCOLMHelper(_In_ unsigned int const uiColumns)
{
    bool fSuccess = SetColumns(uiColumns);
    if (fSuccess)
    {
        fSuccess = CursorPosition(1, 1);
        if (fSuccess)
        {
            fSuccess = EraseInDisplay(EraseType::All);
            if (fSuccess)
            {
                fSuccess = SetTopBottomScrollingMargins(0, 0);
            }
        }
    }
    return fSuccess;
}

bool AdaptDispatch::_PrivateModeParamsHelper(_In_ PrivateModeParams const param, _In_ bool const fEnable)
{
    bool fSuccess = false;
    switch(param)
    {
    case PrivateModeParams::DECCKM_CursorKeysMode:
        // set - Enable Application Mode, reset - Normal mode
        fSuccess = SetCursorKeysMode(fEnable);
        break;
    case PrivateModeParams::DECCOLM_SetNumberOfColumns:
        fSuccess = _DoDECCOLMHelper(fEnable? s_sDECCOLMSetColumns : s_sDECCOLMResetColumns);
        break;
    case PrivateModeParams::ATT610_StartCursorBlink:
        fSuccess = EnableCursorBlinking(fEnable);
        break;
    case PrivateModeParams::DECTCEM_TextCursorEnableMode:
        fSuccess = CursorVisibility(fEnable);
        break;
    case PrivateModeParams::VT200_MOUSE_MODE:
        fSuccess = EnableVT200MouseMode(fEnable);
        break;
    case PrivateModeParams::BUTTTON_EVENT_MOUSE_MODE:
        fSuccess = EnableButtonEventMouseMode(fEnable);
        break;
    case PrivateModeParams::ANY_EVENT_MOUSE_MODE:
        fSuccess = EnableAnyEventMouseMode(fEnable);
        break;
    case PrivateModeParams::UTF8_EXTENDED_MODE:
        fSuccess = EnableUTF8ExtendedMouseMode(fEnable);
        break;
    case PrivateModeParams::SGR_EXTENDED_MODE:
        fSuccess = EnableSGRExtendedMouseMode(fEnable);
        break;
    case PrivateModeParams::ALTERNATE_SCROLL:
        fSuccess = EnableAlternateScroll(fEnable);
        break;
    case PrivateModeParams::ASB_AlternateScreenBuffer:
        fSuccess = fEnable? UseAlternateScreenBuffer() : UseMainScreenBuffer();
        break;
    default:
        // If no functions to call, overall dispatch was a failure.
        fSuccess = false;
        break;
    }
    return fSuccess;
}

// Routine Description:
// - Generalized handler for the setting/resetting of DECSET/DECRST parameters.
//     All params in the rgParams will attempt to be executed, even if one
//     fails, to allow us to successfully re/set params that are chained with
//     params we don't yet support.
// Arguments:
// - rgParams - array of params to set/reset
// - cParams - length of rgParams
// Return Value:
// - True if ALL params were handled successfully. False otherwise.
bool AdaptDispatch::_SetResetPrivateModes(_In_reads_(cParams) const PrivateModeParams* const rgParams, _In_ size_t const cParams, _In_ bool const fEnable)
{
    // because the user might chain together params we don't support with params we DO support, execute all
    // params in the sequence, and only return failure if we failed at least one of them
    size_t cFailures = 0;
    for (size_t i = 0; i < cParams; i++)
    {
        cFailures += _PrivateModeParamsHelper(rgParams[i], fEnable)? 0 : 1; // increment the number of failures if we fail.
    }
    return cFailures == 0;
}

// Routine Description:
// - DECSET - Enables the given DEC private mode params.
// Arguments:
// - rgParams - array of params to set
// - cParams - length of rgParams
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetPrivateModes(_In_reads_(cParams) const PrivateModeParams* const rgParams, _In_ size_t const cParams)
{
    return _SetResetPrivateModes(rgParams, cParams, true);
}

// Routine Description:
// - DECRST - Disables the given DEC private mode params.
// Arguments:
// - rgParams - array of params to reset
// - cParams - length of rgParams
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::ResetPrivateModes(_In_reads_(cParams) const PrivateModeParams* const rgParams, _In_ size_t const cParams)
{
    return _SetResetPrivateModes(rgParams, cParams, false);
}

// - DECKPAM, DECKPNM - Sets the keypad input mode to either Application mode or Numeric mode (true, false respectively)
// Arguments:
// - fApplicationMode - set to true to enable Application Mode Input, false for Numeric Mode Input.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetKeypadMode(_In_ bool const fApplicationMode)
{
    return !!_pConApi->PrivateSetKeypadMode(fApplicationMode);
}

// - DECCKM - Sets the cursor keys input mode to either Application mode or Normal mode (true, false respectively)
// Arguments:
// - fApplicationMode - set to true to enable Application Mode Input, false for Normal Mode Input.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetCursorKeysMode(_In_ bool const fApplicationMode)
{
    return !!_pConApi->PrivateSetCursorKeysMode(fApplicationMode);
}

// - att610 - Enables or disables the cursor blinking.
// Arguments:
// - fEnable - set to true to enable blinking, false to disable
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::EnableCursorBlinking(_In_ bool const fEnable)
{
    return !!_pConApi->PrivateAllowCursorBlinking(fEnable);
}

// Routine Description:
// - Generalizes inserting and deleting lines for IL and DL sequences.
// Arguments:
// - uiDistance - Magnitude of the insert/delete
// - fInsert - true for insert lines, false for delete.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::_InsertDeleteLines(_In_ unsigned int const uiDistance, _In_ bool const fInsert) const
{
    // We'll be doing short math on the distance since all console APIs use shorts. So check that we can successfully convert the uint into a short first.
    SHORT sDistance;
    bool fSuccess = SUCCEEDED(UIntToShort(uiDistance, &sDistance));

    if (fSuccess)
    {
        // get current cursor
        CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
        csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
        fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

        if (fSuccess)
        {
            SMALL_RECT Screen = csbiex.srWindow;
            COORD Cursor = csbiex.dwCursorPosition;

            // Rectangle to cut out of the existing buffer
            SMALL_RECT srScroll;
            srScroll.Left = 0;
            srScroll.Right = Screen.Right - Screen.Left;
            srScroll.Top = Cursor.Y;
            srScroll.Bottom = Screen.Bottom;
            // Paste coordinate for cut text above
            COORD coordDestination;
            coordDestination.X = 0;
            coordDestination.Y = (Cursor.Y) + (sDistance * (fInsert? 1 : -1));

            SMALL_RECT srClip = csbiex.srWindow;
            srClip.Top = Cursor.Y;

            // Fill character for remaining space left behind by "cut" operation (or for fill if we "cut" the entire line)
            CHAR_INFO ciFill;
            ciFill.Attributes = csbiex.wAttributes;
            ciFill.Char.UnicodeChar = L' ';
            fSuccess = !!_pConApi->ScrollConsoleScreenBufferW(&srScroll, &srClip, coordDestination, &ciFill);
        }
    }

    return fSuccess;
}

// Routine Description:
// - IL - This control function inserts one or more blank lines, starting at the cursor.
//    As lines are inserted, lines below the cursor and in the scrolling region move down.
//    Lines scrolled off the page are lost. IL has no effect outside the page margins.
// Arguments:
// - uiDistance - number of lines to insert
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::InsertLine(_In_ unsigned int const uiDistance)
{
    return _InsertDeleteLines(uiDistance, true);
}

// Routine Description:
// - DL - This control function deletes one or more lines in the scrolling
//    region, starting with the line that has the cursor.
//    As lines are deleted, lines below the cursor and in the scrolling region
//    move up. The terminal adds blank lines with no visual character
//    attributes at the bottom of the scrolling region. If uiDistance is greater than
//    the number of lines remaining on the page, DL deletes only the remaining
//    lines. DL has no effect outside the scrolling margins.
// Arguments:
// - uiDistance - number of lines to delete
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::DeleteLine(_In_ unsigned int const uiDistance)
{
    return _InsertDeleteLines(uiDistance, false);
}

// Routine Description:
// - DECSTBM - Set Scrolling Region
// This control function sets the top and bottom margins for the current page.
//  You cannot perform scrolling outside the margins.
//  Default: Margins are at the page limits.
// Arguments:
// - sTopMargin - the line number for the top margin.
// - sBottomMargin - the line number for the bottom margin.
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetTopBottomScrollingMargins(_In_ SHORT const sTopMargin, _In_ SHORT const sBottomMargin)
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);

    // so notes time: (input -> state machine out -> adapter out -> conhost internal)
    // having only a top param is legal         ([3;r   -> 3,0   -> 3,h  -> 3,h,true)
    // having only a bottom param is legal      ([;3r   -> 0,3   -> 1,3  -> 1,3,true)
    // having neither uses the defaults         ([;r [r -> 0,0   -> 0,0  -> 0,0,false)
    // an illegal combo (eg, 3;2r) is ignored
    if (fSuccess)
    {
        SHORT sActualTop = sTopMargin;
        SHORT sActualBottom = sBottomMargin;
        SHORT sScreenHeight = csbiex.srWindow.Bottom - csbiex.srWindow.Top;
        if ( sActualTop == 0 && sActualBottom == 0)
        {
            // Disable Margins
            // This case is valid, and nothing changes.
        }
        else if (sActualBottom == 0)
        {
            sActualBottom = sScreenHeight;
        }
        else if (sActualBottom < sActualTop)
        {
            fSuccess = false;
        }
        else if ((sActualTop == 0 || sActualTop == 1) && sActualBottom == sScreenHeight)
        {
            // Client requests setting margins to the entire screen
            //    - clear them instead of setting them.
            // This is for apps like `apt` (NOT `apt-get` which set scroll
            //      margins, but don't use the alt buffer.)
            // Some apps will use 0 as a top, some will use 1. Both should behave the same.
            sActualBottom = 0;
        }
        // In VT, the origin is 1,1. For our array, it's 0,0. So subtract 1.
        if (sActualTop > 0)
        {
            sActualTop -= 1;
        }
        if (sActualBottom > 0)
        {
            sActualBottom -= 1;
        }
        if (fSuccess)
        {
            _srScrollMargins.Top = sActualTop;
            _srScrollMargins.Bottom = sActualBottom;
            fSuccess = !!_pConApi->PrivateSetScrollingRegion(&_srScrollMargins);
            if (fSuccess)
            {
                this->CursorPosition(1,1);
            }
        }
    }
    return fSuccess;
}

// Routine Description:
// - RI - Performs a "Reverse line feed", essentially, the opposite of '\n'.
//    Moves the cursor up one line, and tries to keep its position in the line
// Arguments:
// - None
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::ReverseLineFeed()
{
    return !!_pConApi->PrivateReverseLineFeed();
}

// Routine Description:
// - OSC Set Window Title - Sets the title of the window
// Arguments:
// - pwchWindowTitle - The string to set the title to. Must be null terminated.
// - cchTitleLength - The length of the title string specified by pwchWindowTitle
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::SetWindowTitle(_In_ const wchar_t* const pwchWindowTitle, _In_ unsigned short cchTitleLength)
{
    return !!_pConApi->SetConsoleTitleW(pwchWindowTitle, cchTitleLength);
}

// - ASBSET - Creates and swaps to the alternate screen buffer. In virtual terminals, there exists both a "main"
//     screen buffer and an alternate. ASBSET creates a new alternate, and switches to it. If there is an already
//     existing alternate, it is discarded.
// Arguments:
// - None
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::UseAlternateScreenBuffer()
{
    return !!_pConApi->PrivateUseAlternateScreenBuffer();
}

// Routine Description:
// - ASBRST - From the alternate buffer, returns to the main screen buffer.
//     From the main screen buffer, does nothing. The alternate is discarded.
// Arguments:
// - None
// Return Value:
// - True if handled successfully. False otherwise.
bool AdaptDispatch::UseMainScreenBuffer()
{
    return !!_pConApi->PrivateUseMainScreenBuffer();
}

//Routine Description:
// HTS - sets a VT tab stop in the cursor's current column.
//Arguments:
// - None
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::HorizontalTabSet()
{
    return !!_pConApi->PrivateHorizontalTabSet();
}

//Routine Description:
// CHT - performing a forwards tab. This will take the
//     cursor to the tab stop following its current location. If there are no
//     more tabs in this row, it will take it to the right side of the window.
//     If it's already in the last column of the row, it will move it to the next line.
//Arguments:
// - sNumTabs - the number of tabs to perform
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::ForwardTab(_In_ SHORT const sNumTabs)
{
    return !!_pConApi->PrivateForwardTab(sNumTabs);
}

//Routine Description:
// CBT - performing a backwards tab. This will take the cursor to the tab stop
//     previous to its current location. It will not reverse line feed.
//Arguments:
// - sNumTabs - the number of tabs to perform
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::BackwardsTab(_In_ SHORT const sNumTabs)
{
    return !!_pConApi->PrivateBackwardsTab(sNumTabs);
}

//Routine Description:
// TBC - Used to clear set tab stops. ClearType ClearCurrentColumn (0) results
//     in clearing only the tab stop in the cursor's current column, if there
//     is one. ClearAllColumns (3) results in resetting all set tab stops.
//Arguments:
// - sClearType - Whether to clear the current column, or all columns, defined in TermDispatch::TabClearType
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::TabClear(_In_ SHORT const sClearType)
{
    bool fSuccess = false;
    switch (sClearType)
    {
    case TermDispatch::TabClearType::ClearCurrentColumn:
        fSuccess = !!_pConApi->PrivateTabClear(false);
        break;
    case TermDispatch::TabClearType::ClearAllColumns:
        fSuccess = !!_pConApi->PrivateTabClear(true);
        break;
    }
    return fSuccess;
}

//Routine Description:
// Designate Charset - Sets the active charset to be the one mapped to wch.
//     See TermDispatch::VTCharacterSets for a list of supported charsets.
//     Also http://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Controls-beginning-with-ESC
//       For a list of all charsets and their codes.
//     If the specified charset is unsupported, we do nothing (remain on the current one)
//Arguments:
// - wchCharset - The character indicating the charset we should switch to.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::DesignateCharset(_In_ wchar_t const wchCharset)
{
    return _TermOutput.DesignateCharset(wchCharset);
}

//Routine Description:
// Soft Reset - Perform a soft reset. See http://www.vt100.net/docs/vt510-rm/DECSTR.html
// The following table lists everything that should be done, 'X's indicate the ones that
//   we actually perform. As the appropriate functionality is added to our ANSI support,
//   we should update this.
//  X Text cursor enable          DECTCEM     Cursor enabled.
//    Insert/replace              IRM         Replace mode.
//    Origin                      DECOM       Absolute (cursor origin at upper-left of screen.)
//    Autowrap                    DECAWM      No autowrap.
//    National replacement        DECNRCM     Multinational set.
//        character set
//    Keyboard action             KAM         Unlocked.
//  X Numeric keypad              DECNKM      Numeric characters.
//  X Cursor keys                 DECCKM      Normal (arrow keys).
//  X Set top and bottom margins  DECSTBM     Top margin = 1; bottom margin = page length.
//  X All character sets          G0, G1, G2, Default settings.
//                                G3, GL, GR
//  X Select graphic rendition    SGR         Normal rendition.
//    Select character attribute  DECSCA      Normal (erasable by DECSEL and DECSED).
//  X Save cursor state           DECSC       Home position.
//    Assign user preference      DECAUPSS    Set selected in Set-Up.
//        supplemental set
//    Select active               DECSASD     Main display.
//        status display
//    Keyboard position mode      DECKPM      Character codes.
//    Cursor direction            DECRLM      Reset (Left-to-right), regardless of NVR setting.
//    PC Term mode                DECPCTERM   Always reset.
//Arguments:
// <none>
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::SoftReset()
{
    bool fSuccess = CursorVisibility(true); // Cursor enabled.
    if (fSuccess)
    {
        fSuccess = SetCursorKeysMode(false); // Normal characters.
    }
    if (fSuccess)
    {
        fSuccess = SetKeypadMode(false); // Numeric characters.
    }
    if (fSuccess)
    {
        fSuccess = SetTopBottomScrollingMargins(0, 0); // Top margin = 1; bottom margin = page length.
    }
    if (fSuccess)
    {
        fSuccess = DesignateCharset(TermDispatch::VTCharacterSets::USASCII); // Default Charset
    }
    if (fSuccess)
    {
        GraphicsOptions opt = GraphicsOptions::Off;
        fSuccess = SetGraphicsRendition(&opt, 1); // Normal rendition.
    }
    // SetTopBottomMargins already moved the cursor to 1,1
    if (fSuccess)
    {
        fSuccess = CursorSavePosition(); // Home position.
    }

    return fSuccess;
}

//Routine Description:
// Full Reset - Perform a hard reset of the terminal. http://vt100.net/docs/vt220-rm/chapter4.html
//  RIS performs the following actions: (Items with sub-bullets are supported)
//   - Performs a communications line disconnect.
//   - Clears UDKs.
//   - Clears a down-line-loaded character set.
//   - Clears the screen.
//      * This is like Erase in Display (3), also clearing scrollback, as well as ED(2)
//   - Returns the cursor to the upper-left corner of the screen.
//      * CUP(1;1)
//   - Sets the SGR state to normal.
//      * SGR(Off)
//   - Sets the selective erase attribute write state to "not erasable".
//   - Sets all character sets to the default.
//      * G0(USASCII)
//Arguments:
// <none>
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::HardReset()
{
    // Clears the screen - Needs to be done in two operations.
    bool fSuccess = _EraseScrollback();
    if (fSuccess)
    {
        fSuccess = EraseInDisplay(EraseType::All);
    }

    // Cursor to 1,1
    if (fSuccess)
    {
        fSuccess = CursorPosition(1, 1);
    }

    // Sets the SGR state to normal.
    if (fSuccess)
    {
        GraphicsOptions opt = GraphicsOptions::Off;
        fSuccess = SetGraphicsRendition(&opt, 1); // Normal rendition.
    }

    if (fSuccess)
    {
        fSuccess = DesignateCharset(TermDispatch::VTCharacterSets::USASCII); // Default Charset
    }

    return fSuccess;
}

//Routine Description:
// Erase Scrollback (^[[3J - ED extension by xterm)
//  Because conhost doesn't exactly have a scrollback, We have to be tricky here.
//  We need to move the entire viewport to 0,0, and clear everything outside
//      (0, 0, viewportWidth, viewportHeight) To give the appearance that
//      everything above the viewport was cleared.
//  We don't want to save the text BELOW the viewport, because in *nix, there isn't anything there
//      (There isn't a scroll-forward, only a scrollback)
//Arguments:
// <none>
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::_EraseScrollback()
{
    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    bool fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);
    if (fSuccess)
    {
        const SMALL_RECT Screen = csbiex.srWindow;
        const short sWidth = Screen.Right - Screen.Left;
        const short sHeight = Screen.Bottom - Screen.Top;
        assert(sWidth > 0 && sHeight > 0);
        const COORD Cursor = csbiex.dwCursorPosition;

        // Rectangle to cut out of the existing buffer
        SMALL_RECT srScroll = Screen;
        // Paste coordinate for cut text above
        COORD coordDestination;
        coordDestination.X = 0;
        coordDestination.Y = 0;

        // Fill character for remaining space left behind by "cut" operation (or for fill if we "cut" the entire line)
        CHAR_INFO ciFill;
        ciFill.Attributes = csbiex.wAttributes;
        ciFill.Char.UnicodeChar = static_cast<WCHAR>(0x20); // space character. use 0x20 instead of literal space because we can't assume the compiler will always turn ' ' into 0x20.
        fSuccess = !!_pConApi->ScrollConsoleScreenBufferW(&srScroll, nullptr, coordDestination, &ciFill);
        if (fSuccess)
        {
            // Clear everything after the viewport.

            // This is two regions, below the viewport and to the right of the viewport.
            const DWORD dwTotalAreaBelow = csbiex.dwSize.X * (csbiex.dwSize.Y - sHeight);
            const COORD coordBelowStartPosition = {0, sHeight};
            // We don't use the _EraseAreaHelper here because _EraseSingleLineDistanceHelper does it all in one operation
            fSuccess = _EraseSingleLineDistanceHelper(coordBelowStartPosition, dwTotalAreaBelow, csbiex.wAttributes);

            if (fSuccess)
            {
                const COORD coordBottomRight = {csbiex.dwSize.X, coordBelowStartPosition.Y};

                const COORD coordRightStartPosition = {sWidth, 0};
                // We use the Area helper here because the Line helper would
                //      erase the parts of the screen we want to keep too
                fSuccess = _EraseAreaHelper(coordRightStartPosition, coordBottomRight, csbiex.wAttributes);

                if (fSuccess)
                {
                    // Move the viewport (CAN'T be done in one call with SetConsoleScreenBufferInfoEx, because legacy)
                    SMALL_RECT srNewViewport;
                    srNewViewport.Left = 0;
                    srNewViewport.Top = 0;
                    // SetConsoleWindowInfo uses an inclusive rect, while GetConsoleScreenBufferInfo is exclusive
                    srNewViewport.Right = sWidth - 1;
                    srNewViewport.Bottom = sHeight - 1;
                    fSuccess = !!_pConApi->SetConsoleWindowInfo(true, &srNewViewport);

                    if (fSuccess)
                    {
                        // Move the cursor to the same relative location.
                        const COORD newCursor = {Cursor.X-Screen.Left, Cursor.Y-Screen.Top};
                        fSuccess = !!_pConApi->SetConsoleCursorPosition(newCursor);
                    }
                }
            }
        }
    }
    return fSuccess;
}

//Routine Description:
// Erase All (^[[2J - ED)
//  Erase the current contents of the viewport. In most terminals, because they
//      only have a scrollback (and not a buffer per-se), they implement this
//      by scrolling the current contents of the buffer off of the screen.
//  We can't properly replicate this behavior with only the public API, because
//      we need to know where the last character in the buffer is. (it may be below the viewport)
//Arguments:
// <none>
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::_EraseAll()
{
    return !!_pConApi->PrivateEraseAll();
}

//Routine Description:
// Enable VT200 Mouse Mode - Enables/disables the mouse input handler in default tracking mode.
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableVT200MouseMode(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableVT200MouseMode(fEnabled);
}

//Routine Description:
// Enable UTF-8 Extended Encoding - this changes the encoding scheme for sequences
//      emitted by the mouse input handler. Does not enable/disable mouse mode on it's own.
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableUTF8ExtendedMouseMode(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableUTF8ExtendedMouseMode(fEnabled);
}

//Routine Description:
// Enable SGR Extended Encoding - this changes the encoding scheme for sequences
//      emitted by the mouse input handler. Does not enable/disable mouse mode on it's own.
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableSGRExtendedMouseMode(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableSGRExtendedMouseMode(fEnabled);
}

//Routine Description:
// Enable Button Event mode - send mouse move events WITH A BUTTON PRESSED to the input.
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableButtonEventMouseMode(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableButtonEventMouseMode(fEnabled);
}

//Routine Description:
// Enable Any Event mode - send all mouse events to the input.
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableAnyEventMouseMode(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableAnyEventMouseMode(fEnabled);
}

//Routine Description:
// Enable Alternate Scroll Mode - When in the Alt Buffer, send CUP and CUD on
//      scroll up/down events instead of the usual sequences
//Arguments:
// - fEnabled - true to enable, false to disable.
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::EnableAlternateScroll(_In_ bool const fEnabled)
{
    return !!_pConApi->PrivateEnableAlternateScroll(fEnabled);
}

//Routine Description:
// Set Cursor Style - 
//Arguments:
// - cursorStyle - 
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::SetCursorStyle(_In_ DispatchCommon::CursorStyle cursorStyle)
{
    Cursor::CursorType actualType = Cursor::CursorType::Legacy;
    bool fEnableBlinking = false;

    switch(cursorStyle){
    case DispatchCommon::CursorStyle::BlinkingBlock:
    case DispatchCommon::CursorStyle::BlinkingBlockDefault:
        fEnableBlinking = true;
        actualType = Cursor::CursorType::FullBox;
        break;
    case DispatchCommon::CursorStyle::SteadyBlock:
        fEnableBlinking = false;
        actualType = Cursor::CursorType::FullBox;
        break;

    case DispatchCommon::CursorStyle::BlinkingUnderline:
        fEnableBlinking = true;
        actualType = Cursor::CursorType::Underscore;
        break;
    case DispatchCommon::CursorStyle::SteadyUnderline:
        fEnableBlinking = false;
        actualType = Cursor::CursorType::Underscore;
        break;

    case DispatchCommon::CursorStyle::BlinkingBar:
        fEnableBlinking = true;
        actualType = Cursor::CursorType::VerticalBar;
        break;
    case DispatchCommon::CursorStyle::SteadyBar:
        fEnableBlinking = false;
        actualType = Cursor::CursorType::VerticalBar;
        break;
    }

    bool fSuccess = !!_pConApi->SetCursorStyle((unsigned int)actualType);
    if (fSuccess)
    {
        fSuccess = !!_pConApi->PrivateAllowCursorBlinking(fEnableBlinking);
    }

    return fSuccess;
}

// Method Description:
// - Sets a single entry of the colortable to a new value
// Arguments:
// - tableIndex: The VT color table index
// - dwColor: The new RGB color value to use.
// Return Value:
// True if handled successfully. False othewise.
bool AdaptDispatch::SetColorTableEntry(_In_ const size_t tableIndex,
                                       _In_ const DWORD dwColor)
{
    bool fSuccess = tableIndex < 16;
    if (fSuccess)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
        csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
        fSuccess = !!_pConApi->GetConsoleScreenBufferInfoEx(&csbiex);
        if (fSuccess)
        {
            size_t realIndex = ::XtermToWindowsIndex(tableIndex);

            csbiex.ColorTable[realIndex] = dwColor;
            fSuccess = !!_pConApi->SetConsoleScreenBufferInfoEx(&csbiex);
        }
    }
    return fSuccess;
}

//Routine Description:
// Window Manipulation - Performs a variety of actions relating to the window,
//      such as moving the window position, resizing the window, querying 
//      window state, forcing the window to repaint, etc.
//  This is kept seperate from the input version, as there may be
//      codes that are supported in one direction but not the other.
//Arguments:
// - uiFunction - An identifier of the WindowManipulation function to perform
// - rgusParams - Additional parameters to pass to the function
// - cParams - size of rgusParams
// Return value:
// True if handled successfully. False othewise.
bool AdaptDispatch::WindowManipulation(_In_ const DispatchCommon::WindowManipulationType uiFunction,
                                       _In_reads_(cParams) const unsigned short* const rgusParams,
                                       _In_ size_t const cParams)
{
    bool fSuccess = false;
    // Other Window Manipulation functions:
    //  MSFT:13271098 - QueryViewport
    //  MSFT:13271146 - QueryScreenSize
    //  MSFT:14179497 - RefreshWindow
    switch (uiFunction)
    {
        case DispatchCommon::WindowManipulationType::ResizeWindowInCharacters:
            if (cParams == 2)
            {
                fSuccess = DispatchCommon::s_ResizeWindow(_pConApi, rgusParams[1], rgusParams[0]);
            }
            break;
        default:
            fSuccess = false;
    }

    return fSuccess;
}
