/***
*frame.cpp - The frame handler and everything associated with it.
*
*       Copyright (c) Microsoft Corporation. All rights reserved.
*
*Purpose:
*       The frame handler and everything associated with it.
*
*       Entry points:
*       _CxxFrameHandler   - the frame handler.
*
*       Open issues:
*         Handling re-throw from dynamicly nested scope.
*         Fault-tolerance (checking for data structure validity).
****/

#include <eh.h>         // User-visible routines for eh
#include <ehassert.h>   // This project's versions of standard assert macros
#include <ehdata.h>     // Declarations of all types used for EH
#include <ehdata4.h>
#include <ehhooks.h>    // Declarations of hook variables and callbacks
#include <trnsctrl.h>   // Routines to handle transfer of control (trnsctrl.asm)
#include <vcruntime_exception.h>
#include <vcruntime_typeinfo.h>

#include <Unknwn.h>
#include <Windows.h>
#include "ehhelpers.h"

// The CallUnexpected function is only called in some build configurations.
#pragma warning(disable: 4505) // unreferenced local function has been removed
#pragma warning(disable: 4702) // unreachable code

#define cxxReThrow   (__vcrt_getptd()->_cxxReThrow)


////////////////////////////////////////////////////////////////////////////////
//
// Intel x86-specific definitions
//
#if defined(_M_IX86) && !defined(_CHPE_X86_ARM64_EH_)

#if 0
void RENAME_EH_EXTERN(__FrameHandler3)::FrameUnwindToEmptyState(
    EHRegistrationNode *pRN,
    DispatcherContext  *pDC,
    FuncInfo           *pFuncInfo
)
{
    FrameUnwindToState(pRN, pDC, pFuncInfo, EH_EMPTY_STATE);
}
#endif

#define __GetAddress(a, b)      (a)
#define REAL_FP(a, b)           (a)

////////////////////////////////////////////////////////////////////////////////
//
// x64/arm/arm64-specific definitions
//
#elif defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)

#define __GetAddress(a, b)      (void*)(a)
#define REAL_FP(a, b)           (a)

#define FUNC_ESTYPES(fi)        ((fi).dispESTypeList ? FUNC_ESTYPES_IB(fi, _GetImageBase()) : nullptr )
#define FUNC_PESTYPES(fi)       ((*(fi)).dispESTypeList ? FUNC_PESTYPES_IB((fi), _GetImageBase()) : nullptr )

#if _EH_RELATIVE_TYPEINFO

// The throw site
#undef CT_PTD
#define CT_PTD(ct)              (CT_PTD_IB(ct, _GetThrowImageBase()))
#undef CT_COPYFUNC
#define CT_COPYFUNC(ct)         ((ct).copyFunction? CT_COPYFUNC_IB(ct, _GetThrowImageBase()):nullptr)

#define EST_ARRAY(x,n)          EST_ARRAY_IB(x, _GetImageBase(), n)

#undef THROW_FORWARDCOMPAT
#define THROW_FORWARDCOMPAT(ti) ((ti).pForwardCompat? THROW_FORWARDCOMPAT_IB(ti, _GetThrowImageBase()):nullptr)

#endif // _EH_RELATIVE_TYPEINFO

// The catch site
#undef HT_HANDLER
#define HT_HANDLER(ht)          (HT_HANDLER_IB(ht, _GetImageBase()))
#undef UWE_ACTION
#define UWE_ACTION(uwe)         ((uwe).action? UWE_ACTION_IB(uwe, _GetImageBase()):nullptr)

#undef FUNC_UNWIND
#define FUNC_UNWIND(fi,st)      (FUNC_PUNWINDMAP(fi,_GetImageBase())[st])
#undef TBME_CATCH
#define TBME_CATCH(hm,n)        (TBME_PLIST(hm,_GetImageBase())[n])
#undef TBME_PCATCH
#define TBME_PCATCH(hm,n)       (&(TBME_PLIST(hm,_GetImageBase())[n]))

#undef HT_PTD
#define HT_PTD(ht)              ((TypeDescriptor*)((ht).dispType? HT_PTD_IB(ht,_GetImageBase()):nullptr))

#undef abnormal_termination
#define abnormal_termination()  FALSE

#else
#error Unrecognized platform
#endif

extern "C" {
typedef struct {
    unsigned long dwSig;
    unsigned long uoffDestination;
    unsigned long dwCode;
    unsigned long uoffFramePointer;
} _NLG_INFO;

extern _NLG_INFO _NLG_Destination;
}

////////////////////////////////////////////////////////////////////////////////
//
// Forward declaration of local functions:
//

template <class T>
static void FindHandler(
    EHExceptionRecord *,
    EHRegistrationNode *,
    CONTEXT *,
    DispatcherContext *,
    typename T::FuncInfo *,
    BOOLEAN,
    int,
    EHRegistrationNode*
);

template <class T>
static void CatchIt(
    EHExceptionRecord *,
    EHRegistrationNode *,
    CONTEXT *,
    DispatcherContext *,
    typename T::FuncInfo *,
    typename T::HandlerType *,
    CatchableType *,
    typename T::TryBlockMapEntry *,
    int,
    EHRegistrationNode *,
    BOOLEAN
#if defined (_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    , BOOLEAN
#endif
);

static void * CallCatchBlock(
    EHExceptionRecord *,
    EHRegistrationNode *,
    CONTEXT *,
    FuncInfo *,
    void *,
    int,
    unsigned long
);

template <class T>
static void FindHandlerForForeignException(
    EHExceptionRecord *,
    EHRegistrationNode *, CONTEXT *,
    DispatcherContext *,
    typename T::FuncInfo *,
    __ehstate_t,
    int,
    EHRegistrationNode *
);

static int ExFilterRethrow(
    EXCEPTION_POINTERS *
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    ,EHExceptionRecord *,
    int *
#endif
);

static BOOLEAN IsInExceptionSpec(
    EHExceptionRecord *pExcept,         // Information for this (logical)
                                        //   exception
    ESTypeList *pFuncInfo               // Static information for subject frame
);

static void CallUnexpected(ESTypeList* pESTypeList);

static BOOLEAN Is_bad_exception_allowed(ESTypeList *pExceptionSpec);

//
// This describes the most recently handled exception, in case of a rethrow:
//
#define _pCurrentFuncInfo       (*((ESTypeList **)&(__vcrt_getptd()->_curexcspec)))

inline ESTypeList* RENAME_EH_EXTERN(__FrameHandler3)::getESTypes(FuncInfo* pFuncInfo)
{
    return FUNC_PESTYPES(pFuncInfo);
}

////////////////////////////////////////////////////////////////////////////////
//
// __InternalCxxFrameHandler - the frame handler for all functions with C++ EH
// information.
//
// If exception is handled, this doesn't return; otherwise, it returns
// ExceptionContinueSearch.
//
// Note that this is called three ways:
//     From __CxxFrameHandler: primary usage, called to inspect whole function.
//         CatchDepth == 0, pMarkerRN == nullptr
//     From CatchGuardHandler: If an exception occurred within a catch, this is
//         called to check for try blocks within that catch only, and does not
//         handle unwinds.
//     From TranslatorGuardHandler: Called to handle the translation of a
//         non-C++ EH exception.  Context considered is that of parent.

#if 0
template <class T>
EXCEPTION_DISPOSITION __InternalCxxFrameHandler(
    EHExceptionRecord  *pExcept,        // Information for this exception
    EHRegistrationNode *pRN,            // Dynamic information for this frame
    CONTEXT *pContext,                  // Context info
    DispatcherContext *pDC,             // Context within subject frame
    typename T::FuncInfo *pFuncInfo,    // Static information for this frame
    int CatchDepth,                     // How deeply nested are we?
    EHRegistrationNode *pMarkerRN,      // Marker node for when checking inside
                                        //  catch block
    BOOLEAN recursive                   // Are we handling a translation?
) {

#if defined(_M_HYBRID_X86_ARM64) && !defined(_CHPE_X86_ARM64_EH_)
    _HybridGenerateThunks(__InternalCxxFrameHandler<T>, 1);
#endif

    EHTRACE_ENTER_FMT2("%s, pRN = 0x%p",
                       IS_UNWINDING(PER_FLAGS(pExcept)) ? "Unwinding" : "Searching",
                       pRN);

    __except_validate_context_record(pContext);

    if ((cxxReThrow == false) && (PER_CODE(pExcept) != EH_EXCEPTION_NUMBER) &&
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID)
        /* On the 64 bit/ARM platforms, ExceptionCode maybe set to STATUS_UNWIND_CONSOLIDATE
           when called from _UnwindNestedFrames during Logical Unwind. _UnwindNestedFrames
           will also set EH_MAGIC_NUMBER1 in the 8 element */
        (!((PER_CODE(pExcept) == STATUS_UNWIND_CONSOLIDATE) && (PER_NPARAMS(pExcept) == 15) && (PER_EXCEPTINFO(pExcept)[8] == EH_MAGIC_NUMBER1))) &&
#endif
        (PER_CODE(pExcept) != STATUS_LONGJUMP) &&
        (T::getMagicNum(pFuncInfo) >= EH_MAGIC_NUMBER3) &&
        (T::isEHs(pFuncInfo)))
    {
        /*
         * This function was compiled /EHs so we don't need to do anything in
         * this handler.
         */
        return ExceptionContinueSearch;
    }

    if (IS_UNWINDING(PER_FLAGS(pExcept)))
    {
        // We're at the unwinding stage of things.  Don't care about the
        // exception itself.  (Check this first because it's easier)

        if (T::GetMaxState(pDC, pFuncInfo) != 0 && CatchDepth == 0)
        {
            // Only unwind if there's something to unwind
            // AND we're being called through the primary RN.

#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)

            if (IS_TARGET_UNWIND(PER_FLAGS(pExcept)) && PER_CODE(pExcept) == STATUS_LONGJUMP) {
                    __ehstate_t target_state = T::StateFromIp(
                                                             pFuncInfo,
                                                             pDC,
#if defined(_M_X64)
                                                             pDC->TargetIp
#elif defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                                             pDC->TargetPc
#endif
                                                             );

                    DASSERT(target_state >= EH_EMPTY_STATE
                            && target_state < T::GetMaxState(pDC, pFuncInfo));

                    T::FrameUnwindToState(pRN, pDC, pFuncInfo, target_state);
                    EHTRACE_HANDLER_EXIT(ExceptionContinueSearch);
                    return ExceptionContinueSearch;
            } else if(IS_TARGET_UNWIND(PER_FLAGS(pExcept)) &&
                      PER_CODE(pExcept) == STATUS_UNWIND_CONSOLIDATE)
            {
                PEXCEPTION_RECORD pSehExcept = (PEXCEPTION_RECORD)pExcept;
                __ehstate_t target_state = (__ehstate_t)pSehExcept->ExceptionInformation[3];

                DASSERT(target_state >= EH_EMPTY_STATE
                        && target_state < T::GetMaxState(pDC, pFuncInfo));
                T::FrameUnwindToState((EHRegistrationNode *)pSehExcept->ExceptionInformation[1],
                                   pDC,
                                   pFuncInfo,
                                   target_state);
                EHTRACE_HANDLER_EXIT(ExceptionContinueSearch);
                return ExceptionContinueSearch;
            }
#endif // defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
            T::FrameUnwindToEmptyState(pRN, pDC, pFuncInfo);
        }
        EHTRACE_HANDLER_EXIT(ExceptionContinueSearch);
        return ExceptionContinueSearch;     // I don't think this value matters

    }
    else
    {
        auto tryBlockMap = T::TryBlockMap(pFuncInfo, pDC);
        if (tryBlockMap.getNumTryBlocks() != 0
            //
            // If the function has no try block, we still want to call the
            // frame handler if there is an exception specification
            //
            || (T::getMagicNum(pFuncInfo) >= EH_MAGIC_NUMBER2 && (T::getESTypes(pFuncInfo) != nullptr))
            || (T::getMagicNum(pFuncInfo) >= EH_MAGIC_NUMBER3 && (T::isNoExcept(pFuncInfo) != 0)))
        {

            // NT is looking for handlers.  We've got handlers.
            // Let's check this puppy out.  Do we recognize it?

            int(__cdecl *pfn)(...);

            if (PER_CODE(pExcept) == EH_EXCEPTION_NUMBER
                && PER_NPARAMS(pExcept) >= 3
                && PER_MAGICNUM(pExcept) > EH_MAGIC_NUMBER3
                && (pfn = THROW_FORWARDCOMPAT(*PER_PTHROW(pExcept))) != nullptr) {

                // Forward compatibility:  The thrown object appears to have been
                // created by a newer version of our compiler.  Let that version's
                // frame handler do the work (if one was specified).

#if defined(_DEBUG)
                if (_ValidateExecute((FARPROC)(PVOID)pfn)) {
#endif
                    EXCEPTION_DISPOSITION result =
                        (EXCEPTION_DISPOSITION)pfn(pExcept, pRN, pContext, pDC,
                            pFuncInfo, CatchDepth,
                            pMarkerRN, recursive);
                    EHTRACE_HANDLER_EXIT(result);
                    return result;
#if defined(_DEBUG)
                }
                else {
                    terminate(); // Does not return; TKB
                }
#endif

            }
            else {

                // Anything else: we'll handle it here.
                FindHandler<T>(pExcept, pRN, pContext, pDC, pFuncInfo, recursive, CatchDepth, pMarkerRN);
            }

            // If it returned, we didn't have any matches.

        } // NT was looking for a handler
    }

    // We had nothing to do with it or it was rethrown.  Keep searching.
    EHTRACE_HANDLER_EXIT(ExceptionContinueSearch);
    return ExceptionContinueSearch;

} // __InternalCxxFrameHandler

template EXCEPTION_DISPOSITION __InternalCxxFrameHandler<RENAME_EH_EXTERN(__FrameHandler3)>(
    EHExceptionRecord  *pExcept,
    EHRegistrationNode *pRN,
    CONTEXT *pContext,
    DispatcherContext *pDC,
    RENAME_EH_EXTERN(__FrameHandler3)::FuncInfo *pFuncInfo,
    int CatchDepth,
    EHRegistrationNode *pMarkerRN,
    BOOLEAN recursive
    );

#if _EH_RELATIVE_FUNCINFO
template EXCEPTION_DISPOSITION __InternalCxxFrameHandler<RENAME_EH_EXTERN(__FrameHandler4)>(
    EHExceptionRecord  *pExcept,
    EHRegistrationNode *pRN,
    CONTEXT *pContext,
    DispatcherContext *pDC,
    RENAME_EH_EXTERN(__FrameHandler4)::FuncInfo *pFuncInfo,
    int CatchDepth,
    EHRegistrationNode *pMarkerRN,
    BOOLEAN recursive
    );
#endif

/*
* Here We find what is the actual State of current function. The way we
* do this is first get State from ControlPc.
*
* Remember we have __GetUnwindTryBlock to remember the last State for which
* Exception was handled and __GetCurrentState for retriving the current
* state of the function. Please Note that __GetCurrentState is used
* primarily for unwinding purpose.
*
* Also remember that all the catch blocks act as funclets. This means that
* ControlPc for all the catch blocks are different from ControlPc of parent
* catch block or function.
*
* take a look at this example
* try {
*   // STATE1 = 1
*   try {
*     // STATE2
*     // THROW
*   } catch (...) { // CatchB1
*     // STATE3
*     // RETHROW OR NEW THROW
*   }
* } catch (...) { // CatchB2
* }
*
* If we have an exception comming from STATE3, the FindHandler will be
* called for CatchB1, at this point we do the test which State is our
* real state, curState from ControlPc or state from __GetUnwindTryBlock.
* Since curState from ControlPc is greater, we know that real State is
* curState from ControlPc and thus we update the UnwindTryBlockState.
*
* On further examination, we found out that there is no handler within
* this catch block, we return without handling the exception. For more
* info on how we determine if we have handler, have a look at
* GetRangeOfTrysToCheck.
*
* Now FindHandler will again be called for parent function. Here again
* we test which is real State, state from ControlPc or State from
* __GetUnwindTryBlock. This time state from __GetUnwindTryBlock is correct.
*
* Also look at code in __CxxCallCatchBlock, you will se that as soon as we get
* out of last catch block, we reset __GetUnwindTryBlock state to -1.
*/

#if _EH_RELATIVE_FUNCINFO
inline __ehstate_t RENAME_EH_EXTERN(__FrameHandler3)::GetHandlerSearchState(
    EHRegistrationNode *pRN,
    DispatcherContext  *pDC,
    FuncInfo           *pFuncInfo
    )
{

    __ehstate_t curState = StateFromControlPc(pFuncInfo, pDC);
    EHRegistrationNode EstablisherFrame;
    GetEstablisherFrame(pRN, pDC, pFuncInfo, &EstablisherFrame);
    if (curState > GetUnwindTryBlock(pRN, pDC, pFuncInfo)) {
        SetState(&EstablisherFrame, pFuncInfo, curState);
        SetUnwindTryBlock(pRN, pDC, pFuncInfo, /*curTry*/ curState);
    }
    else {
      curState = GetUnwindTryBlock(pRN, pDC, pFuncInfo);
    }

     return curState;
}

inline __ehstate_t RENAME_EH_EXTERN(__FrameHandler4)::GetHandlerSearchState(
    EHRegistrationNode* /*pRN*/,
    DispatcherContext   *pDC,
    FuncInfo4           *pFuncInfo
)
{
    __ehstate_t curState = StateFromControlPc(pFuncInfo, pDC);

    if (__vcrt_getptd()->_HandlerSearchState != INVALID_CATCH_SPECIFIC_STATE)
    {
        curState = __vcrt_getptd()->_HandlerSearchState;
        __vcrt_getptd()->_HandlerSearchState = INVALID_CATCH_SPECIFIC_STATE;
    }

    return curState;
}
#endif

template <class T>
static void FindHandler(
    EHExceptionRecord *pExcept,         // Information for this (logical)
                                        //   exception
    EHRegistrationNode *pRN,            // Dynamic information for subject frame
    CONTEXT *pContext,                  // Context info
    DispatcherContext *pDC,             // Context within subject frame
    typename T::FuncInfo *pFuncInfo,    // Static information for subject frame
    BOOLEAN recursive,                  // TRUE if we're handling the
                                        //   translation
    int CatchDepth,                     // Level of nested catch that is being
                                        //   checked
    EHRegistrationNode *pMarkerRN       // Extra marker RN for nested catch
                                        //   handling
)
{

#if defined(_M_HYBRID_X86_ARM64) && !defined(_CHPE_X86_ARM64_EH_)
    _HybridGenerateThunks(FindHandler<T>, 1);
#endif

    EHTRACE_ENTER;

    BOOLEAN IsRethrow = FALSE;
    BOOLEAN gotMatch = FALSE;

    // Get the current state (machine-dependent)
    __ehstate_t curState = EH_EMPTY_STATE;
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    curState = T::GetHandlerSearchState(pRN, pDC, pFuncInfo);
#else
    curState = T::GetCurrentState(pRN, pDC, pFuncInfo);
#endif
    DASSERT(curState >= EH_EMPTY_STATE && curState < T::GetMaxState(pDC, pFuncInfo));

    // Check if it's a re-throw.  Use the exception we stashed away if it is.
    if (PER_IS_MSVC_EH(pExcept) && PER_PTHROW(pExcept) == nullptr) {

        if (_pCurrentException == nullptr) {
            // Oops!  User re-threw a non-existent exception!  Let it propagate.
            EHTRACE_EXIT;
            return;
        }

        pExcept = _pCurrentException;
        pContext = _pCurrentExContext;
        IsRethrow = TRUE;
#if _EH_RELATIVE_TYPEINFO
        _SetThrowImageBase((ptrdiff_t)pExcept->params.pThrowImageBase);
#endif

        DASSERT(_ValidateRead(pExcept));
        DASSERT(!PER_IS_MSVC_EH(pExcept) || PER_PTHROW(pExcept) != nullptr);

        //
        // We know it is a rethrow -- did we come here as a result of an
        // exception re-thrown from CallUnexpected() ?
        //
        if( _pCurrentFuncInfo != nullptr )
        {
            ESTypeList* pCurrentFuncInfo = _pCurrentFuncInfo;   // remember it in a local variable
            _pCurrentFuncInfo = nullptr;   // and reset it immediately -- so we don't forget to do it later

            // Does the exception thrown by CallUnexpected belong to the exception specification?

            if( IsInExceptionSpec(pExcept, pCurrentFuncInfo) )
            {
                // Yes it does -- so "continue the search for another handler at the call of the function
                // whose exception-specification was violated"
                ;
            }
            else
            {
                // Nope, it does not. Is std::bad_exception allowed by the spec?

                if( Is_bad_exception_allowed(pCurrentFuncInfo) )
                {
                    // yup -- so according to the standard, we need to replace the thrown
                    // exception by an implementation-defined object of the type std::bad_exception
                    // and continue the search for another handler at the call of the function
                    // whose exception-specification was violated.

                    // Just throw bad_exception -- we will then come into FindHandler for the third time --
                    // but make sure we will not get here again

                    __DestructExceptionObject(pExcept, TRUE);   // destroy the original object

                    throw std::bad_exception();
                }
                else
                {
                    terminate();
                }
            }
        }
    }

    auto tryBlockMap = T::TryBlockMap(pFuncInfo, pDC);

    if (PER_IS_MSVC_EH(pExcept)) {
        // Looks like it's ours.  Let's see if we have a match:
        //
        // First, determine range of try blocks to consider:
        // Only try blocks which are at the current catch depth are of interest.

        if (tryBlockMap.getNumTryBlocks() > 0)
        {
            auto startStop = T::GetRangeOfTrysToCheck(tryBlockMap, curState, CatchDepth);
            // Scan the try blocks in the function:
            for (auto iter = startStop.first; iter < startStop.second; ++iter) {
                auto tryBlock = *iter;

#if _EH_RELATIVE_TYPEINFO
                __int32 const *ppCatchable;
#else
                CatchableType * const *ppCatchable;
#endif
                CatchableType *pCatchable;

                if (tryBlock->tryLow > curState || curState > tryBlock->tryHigh) {
                    continue;
                }

                // Try block was in scope for current state.  Scan catches for this
                // try:
                T::HandlerMap handlerMap = T::HandlerMap(tryBlock, pDC);
                for (auto handler : handlerMap)
                {
                    // Scan all types that thrown object can be converted to:
                    ppCatchable = THROW_CTLIST(*PER_PTHROW(pExcept));
                    for (int catchables = THROW_COUNT(*PER_PTHROW(pExcept));
                        catchables > 0; catchables--, ppCatchable++) {

#if _EH_RELATIVE_TYPEINFO
                        pCatchable = (CatchableType *)(_GetThrowImageBase() + *ppCatchable);
#else
                        pCatchable = *ppCatchable;
#endif

                        if (!T::TypeMatch(handler, pCatchable, PER_PTHROW(pExcept))) {
                            continue;
                        }

                        // OK.  We finally found a match.  Activate the catch.  If
                        // control gets back here, the catch did a re-throw, so
                        // keep searching.

                        gotMatch = TRUE;

                        CatchIt<T>(pExcept,
                                   pRN,
                                   pContext,
                                   pDC,
                                   pFuncInfo,
                                   handler,
                                   pCatchable,
                                   tryBlock,
                                   CatchDepth,
                                   pMarkerRN,
                                   IsRethrow
#if defined (_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                   , recursive
#endif
                                   );
                        goto NextTryBlock;

                    } // Scan posible conversions
                } // Scan catch clauses
    NextTryBlock: ;
            } // Scan try blocks
        } // if FUNC_NTRYBLOCKS( pFuncInfo ) > 0
#if defined(_DEBUG)
        else
        {
            //
            // This can only happen if the function has an exception specification
            // but no try/catch blocks
            //
            DASSERT(T::getMagicNum(pFuncInfo) >= EH_MAGIC_NUMBER2);
            DASSERT(T::getESTypes(pFuncInfo) != nullptr);
        }
#endif

#if defined(_M_IX86) && !defined(_CHPE_X86_ARM64_EH_)
        if (recursive) {
            //
            // A translation was provided, but this frame didn't catch it.
            // Destruct the translated object before returning; if destruction
            // raises an exception, terminate.
            //
            // This is not done for Win64 platforms.  On those, the translated
            // object is destructed in __CxxCallCatchBlock.
            //
            __DestructExceptionObject(pExcept, TRUE);
        }
#endif

        // FH4 doesn't support Exception Specifications aside from NoExcept
        if constexpr (std::is_same_v<T, RENAME_EH_EXTERN(__FrameHandler4)>)
        {
            if (!gotMatch && T::isNoExcept(pFuncInfo) &&
#if defined(_M_IX86) && !defined(_CHPE_X86_ARM64_EH_)
                CatchDepth == 0
#else
                !T::ExecutionInCatch(pDC, pFuncInfo)
#endif
                )
            {
                terminate();
            }
        }
        else
        {
            //
            // We haven't found the match -- let's look at the exception spec and see if our try
            // matches one of the listed types.
            //
            // This block also handles noexcept termination if the current function is noexcept and
            // we are not handling a nested throw or re-throw. e.g.:
            //
            // try {
            //   try { throw 1; } (1)
            //   catch(int) { throw; /* or throw <something new>; */ } (2)
            // } catch(int) {} (3)
            //
            // During the initial search for a handler when throwing the original exception (1)
            // we find and execute catch handler (2). This throws, which re-enters this function
            // while in the context of a catch, but which cannot find a handler. We cannot terminate here,
            // we're in a recursive context. Just let it go, and we'll return back to the original search
            // from (1) and find (3). If we fail to find a handler, e.g. (3) didn't match, we would
            // then terminate at this point as we are not in a catch block.
            //
            // Catch block detection uses CatchDepth on X86, or _ExecutionInCatch on other platforms. On
            // these platforms CatchDepth is always 0 - we don't maintain a stack of entered try/catch
            // states.
            if (!gotMatch && FUNC_MAGICNUM(*pFuncInfo) >= EH_MAGIC_HAS_ES &&
                (T::getESTypes(pFuncInfo) != nullptr || (T::isNoExcept(pFuncInfo) &&
#if defined(_M_IX86) && !defined(_CHPE_X86_ARM64_EH_)
                    CatchDepth == 0
#else
                    !T::ExecutionInCatch(pDC, pFuncInfo)
#endif
                    )))
            {
                // Are we noexcept?
                if (T::isNoExcept(pFuncInfo))
                {
                    terminate();
                }

                if (!IsInExceptionSpec(pExcept, T::getESTypes(pFuncInfo)))
                {
                    // Nope, it does not. Call unexpected

                    //
                    // We must unwind the stack before calling unexpected -- this makes it work
                    // as if it were inside catch(...) clause
                    //
#if defined (_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                    EHRegistrationNode *pEstablisher = pRN;
                    EHRegistrationNode EstablisherFramePointers;
                    pEstablisher = T::GetEstablisherFrame(pRN, pDC, pFuncInfo, &EstablisherFramePointers);
                    T::UnwindNestedFrames(pRN,
                        pExcept,
                        pContext,
                        pEstablisher,
                        nullptr,
                        pFuncInfo,
                        EH_EMPTY_STATE,
                        EH_EMPTY_STATE,
                        nullptr,
                        pDC,
                        recursive
                    );
#else

                    _pCurrentException = pExcept;
                    _pCurrentExContext = pContext;

                    if (pMarkerRN == nullptr) {
                        RENAME_EH_EXTERN(_UnwindNestedFrames)(pRN, pExcept);
                    }
                    else {
                        RENAME_EH_EXTERN(_UnwindNestedFrames)(pMarkerRN, pExcept);
                    }
                    T::FrameUnwindToEmptyState(pRN, pDC, pFuncInfo);

                    CallUnexpected(T::getESTypes(pFuncInfo));
                    _pCurrentException = pExcept;
                    _pCurrentExContext = pContext;
#endif
                }
            }
        }
    } // It was a C++ EH exception
    else
    {
        // Not ours.  But maybe someone told us how to make it ours.
        if (tryBlockMap.getNumTryBlocks() > 0)
        {
            if (!recursive) {
                FindHandlerForForeignException<T>(pExcept, pRN, pContext, pDC,
                pFuncInfo, curState, CatchDepth, pMarkerRN);
            } else {
                // We're recursive, and the exception wasn't a C++ EH!
                // Translator threw something uninteligable.

                // Two choices here: we could let the new exception take over, or we could abort. We abort.
                terminate();
            }
        }
    } // It wasn't our exception

    DASSERT( _pCurrentFuncInfo == nullptr );   // never leave it initialized with something

    EHTRACE_EXIT;
}

////////////////////////////////////////////////////////////////////////////////
//
// FindHandlerForForeignException - We've got an exception which wasn't ours.
//     Try to translate it into C++ EH, and also check for match with ellipsis.
//
// Description:
//     If an SE-to-EH translator has been installed, call it.  The translator
//     must throw the appropriate typed exception or return.  If the translator
//     throws, we invoke FindHandler again as the exception filter.
//
// Returns:
//     Returns if exception was not fully handled.
//     No return value.
//
// Assumptions:
//     Only called if there are handlers in this function.

template <class T>
static void FindHandlerForForeignException(
    EHExceptionRecord *pExcept,         // Information for this (logical)
                                        //   exception
    EHRegistrationNode *pRN,            // Dynamic information for subject frame
    CONTEXT *pContext,                  // Context info
    DispatcherContext *pDC,             // Context within subject frame
    typename T::FuncInfo *pFuncInfo,    // Static information for subject frame
    __ehstate_t curState,               // Current state
    int catchDepth,                     // Level of nested catch that is being
                                        //   checked
    EHRegistrationNode *pMarkerRN       // Extra marker RN for nested catch
                                        //   handling
    )
{

#if defined(_M_HYBRID_X86_ARM64) && !defined(_CHPE_X86_ARM64_EH_)
    _HybridGenerateThunks(FindHandlerForForeignException<T>, 1);
#endif

    EHTRACE_ENTER;

    // We don't want to touch BreakPoint generated Exception.
    if (PER_CODE(pExcept) == STATUS_BREAKPOINT) {
        EHTRACE_EXIT;
        return;
    }

    if (__pSETranslator != nullptr && __pSETranslator != __vcrt_EncodePointer(nullptr) &&
        pExcept->ExceptionCode != MANAGED_EXCEPTION_CODE &&
        pExcept->ExceptionCode != MANAGED_EXCEPTION_CODE_V4) {

        // Call the translator.  If the translator knows what to
        // make of it, it will throw an appropriate C++ exception.
        // We intercept it and use it (recursively) for this
        // frame.  Don't recurse more than once.

#if _EH_RELATIVE_FUNCINFO
        if (_CallSETranslator<T>(pExcept, pRN, pContext, pDC, pFuncInfo,
            catchDepth, pMarkerRN)) {
#else
        if (_CallSETranslator(pExcept, pRN, pContext, pDC, pFuncInfo,
            catchDepth, pMarkerRN)) {
#endif
            EHTRACE_EXIT;
            return;
        }
    }

    auto tryBlockMap = T::TryBlockMap(pFuncInfo, pDC);

    DASSERT(tryBlockMap.getNumTryBlocks() > 0);

    if (tryBlockMap.getNumTryBlocks() > 0)
    {
        // Didn't have a translator, or the translator returned normally (i.e.
        // didn't translate it).  Still need to check for match with ellipsis:
        auto startStop = T::GetRangeOfTrysToCheck(tryBlockMap, curState, catchDepth);

        // Scan the try blocks in the function:
        for (auto iter = startStop.first; iter < startStop.second; ++iter)
        {
            auto tryBlock = *iter;

            // If the try-block was in scope...
            if (tryBlock->tryLow > curState || curState > tryBlock->tryHigh) {
                continue;
            }

            // *and* the last catch in that try is an ellipsis (no other can be)
            auto handlerMap = T::HandlerMap(tryBlock, pDC);
            auto handler = handlerMap.getLastEntry();
            if (!(HT_IS_TYPE_ELLIPSIS(*handler) && !HT_IS_STD_DOTDOT(*handler))) {
                continue;
            }

            // Found an ellipsis.  Handle exception.
            CatchIt<T>(pExcept,
                pRN,
                pContext,
                pDC,
                pFuncInfo,
                handler,
                nullptr,
                tryBlock,
                catchDepth,
                pMarkerRN,
                TRUE
#if defined (_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                , FALSE
#endif
                );
        }

        // If it returns, handler re-threw.  Keep searching.

    } // Search for try

    EHTRACE_EXIT;

    // If we got here, that means we didn't have anything to do with the
    // exception.  Continue search.
}
#endif

template <class T>
int TypeMatchHelper(
    T *pCatch,                  // Type of the 'catch' clause
    CatchableType *pCatchable,  // Type conversion under consideration
    ThrowInfo *pThrow           // General information about the thrown
                                //   type.
) {
    // First, check for match with ellipsis:
    if (HT_IS_TYPE_ELLIPSIS(*pCatch)) {
        return TRUE;
    }

    if (HT_ISBADALLOCCOMPAT(*pCatch) && CT_ISSTDBADALLOC(*pCatchable))
    {
        return true;
    }

    // Not ellipsis; the basic types match if it's the same record *or* the
    // names are identical.
    if (HT_PTD(*pCatch) != CT_PTD(*pCatchable)
        && strcmp(HT_NAME(*pCatch), CT_NAME(*pCatchable)) != 0) {
        return FALSE;
    }

    // Basic types match.  The actual conversion is valid if:
    //   caught by ref if ref required *and*
    //   the qualifiers are compatible *and*
    //   the alignments match *and*
    //   the volatility matches

    return (!CT_BYREFONLY(*pCatchable) || HT_ISREFERENCE(*pCatch))
        && (!THROW_ISCONST(*pThrow) || HT_ISCONST(*pCatch))
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
        && (!THROW_ISUNALIGNED(*pThrow) || HT_ISUNALIGNED(*pCatch))
#endif
        && (!THROW_ISVOLATILE(*pThrow) || HT_ISVOLATILE(*pCatch));
}

////////////////////////////////////////////////////////////////////////////////
//
// __TypeMatch - Check if the catch type matches the given throw conversion.
//
// Returns:
//     TRUE if the catch can catch using this throw conversion, FALSE otherwise.
extern "C" _VCRTIMP int __cdecl RENAME_EH_EXTERN(__TypeMatch)(
    HandlerType   *pCatch,      // Type of the 'catch' clause
    CatchableType *pCatchable,  // Type conversion under consideration
    ThrowInfo     *pThrow       // General information about the thrown
                                //   type.
    )
{
    return TypeMatchHelper<RENAME_EH_EXTERN(__FrameHandler3)::HandlerType>(pCatch, pCatchable, pThrow);
}

int RENAME_EH_EXTERN(__FrameHandler3)::TypeMatch(
    HandlerType   *pCatch,
    CatchableType *pCatchable,
    ThrowInfo     *pThrow
)
{
    return TypeMatchHelper<HandlerType>(pCatch, pCatchable, pThrow);
};

#if _EH_RELATIVE_FUNCINFO
int RENAME_EH_EXTERN(__FrameHandler4)::TypeMatch(
    HandlerType4  *pCatch,
    CatchableType *pCatchable,
    ThrowInfo     *pThrow
)
{
    return TypeMatchHelper<HandlerType>(pCatch, pCatchable, pThrow);
};
#endif

////////////////////////////////////////////////////////////////////////////////
//
// FrameUnwindToState - Unwind this frame until specified state is reached.
//
// Returns:
//     No return value.
//
// Side Effects:
//     All objects on frame which go out of scope as a result of the unwind are
//       destructed.
//     Registration node is updated to reflect new state.
//
// Usage:
//      This function is called both to do full-frame unwind during the unwind
//      phase (targetState = -1), and to do partial unwinding when the current
//      frame has an appropriate catch.

#if 0
#if _EH_RELATIVE_FUNCINFO
void RENAME_EH_EXTERN(__FrameHandler4)::FrameUnwindToState(
    EHRegistrationNode *pRN,            // Registration node for subject
                                        //   function
    DispatcherContext *pDC,             // Context within subject frame
    FuncInfo4 *pFuncInfo,               // Static information for subject
                                        //   function
    __ehstate_t targetState             // State to unwind to
    )
{
    UNREFERENCED_PARAMETER(pDC); // This parameter is not used in some compilations

    EHTRACE_ENTER;

#if _EH_RELATIVE_FUNCINFO
    ptrdiff_t unwindImageBase = _GetImageBase();
#endif
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    ptrdiff_t curState;
    bool isOffset = false;
    if (__vcrt_getptd()->_UnwindStateorOffset != INVALID_CATCH_SPECIFIC_STATE)
    {
        curState = __vcrt_getptd()->_UnwindStateorOffset;
        isOffset = __vcrt_getptd()->_IsOffset;
    }
    else
    {
        curState = StateFromControlPc(pFuncInfo, pDC);
    }
#else
    DASSERT(!"NYI");
    //__ehstate_t curState = GetCurrentState(pRN, pDC, pFuncInfo);
#endif
    __ProcessingThrow++;

    // Find starting and ending indexes in the decompressed map
    if (pFuncInfo->dispUnwindMap != 0)
    {
        PBYTE buffer = (PBYTE)(pDC->ImageBase + pFuncInfo->dispUnwindMap);
        auto unwindMap = UWMap(buffer);

        UWMap::iterator start = unwindMap.begin();
        UWMap::iterator end = unwindMap.begin();

        if (isOffset)
        {
            start = unwindMap.getOffsetIter(curState);
            end = unwindMap.getStateIter(targetState);
        }
        else
        {
            DASSERT(start >= end);
            unwindMap.getStartStop(static_cast<int>(curState), targetState, start, end);
        }


        __try
        {
            while (start >= unwindMap.begin() && start > end)
            {
                UnwindMapEntry4 UWEntry = *start;
                EHTRACE_DECL(ptrdiff_t startOffset = start - unwindMap.begin());

                ++start;

                // default values taken if UnwindMapEntry4::Type::NoUW
                unsigned dispAction = 0;

                if (UWEntry.type != UnwindMapEntry4::Type::NoUW)
                {
                    dispAction = UWEntry.action;
                }

                if (dispAction != 0) {
                    // Before calling unwind action, adjust state as if it were
                    // already completed:
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                    __vcrt_getptd()->_UnwindStateorOffset = start - unwindMap.begin();
                    __vcrt_getptd()->_IsOffset = true;
#else
                    DASSERT(!"NYI");
                    //SetState(pRN, pDC, pFuncInfo, start - unwindMap.begin());
#endif

                    EHTRACE_FMT2("Unwind from state offset %td to state offset %td", startOffset, start - unwindMap.begin());
                    __try {
                        // Call the unwind action (if one exists):
                        if ((UWEntry.type == UnwindMapEntry4::Type::DtorWithObj) || (UWEntry.type == UnwindMapEntry4::Type::DtorWithPtrToObj))
                        {
#if defined(_M_X64)
                            VOID * objectAddr;
                            if (UWEntry.type == UnwindMapEntry4::Type::DtorWithPtrToObj) {
                                objectAddr = (void*)(*(uintptr_t *)OffsetToAddress(UWEntry.object, *pRN));
                            }
                            else {
                                objectAddr = OffsetToAddress(UWEntry.object, *pRN);
                            }

                            RENAME_EH_EXTERN(_CallSettingFrameEncoded)((void*)(pDC->ImageBase + dispAction),
                                *pRN,
                                 objectAddr,
#if defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                (PULONG)pDC->NonVolatileRegisters,
#endif
                                0x103);

#else
                            DASSERT(!"NYI");
#endif
                        }
                        else
                        {   // TODO: this is currently hardcoded for non-x86, as x86 seems to encode
                            // its RVAs as non-image relative, will require fix for that
                            RENAME_EH_EXTERN(_CallSettingFrame)((void*)(pDC->ImageBase + dispAction),
                                REAL_FP(pRN, pFuncInfo),
#if defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                (PULONG)pDC->NonVolatileRegisters,
#endif
                                0x103);
                        }

#if _EH_RELATIVE_FUNCINFO
                        _SetImageBase(unwindImageBase);
#endif

                    }
                    __except (EHTRACE_EXCEPT(__FrameUnwindFilter(exception_info()))) {
                        ; // Deliberately do nothing
                    }
                }
            }
        }
        __finally {
            if (__ProcessingThrow > 0) {
                __ProcessingThrow--;
            }
        }
    }

    // Now that we're done, set the frame to reflect the final state.
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    __vcrt_getptd()->_UnwindStateorOffset = INVALID_CATCH_SPECIFIC_STATE;
#else
    DASSERT(!"NYI");
#endif

    EHTRACE_FMT2("Moved from state %d to state %d", curState, targetState);
    EHTRACE_EXIT;
}
#endif

void RENAME_EH_EXTERN(__FrameHandler3)::FrameUnwindToState(
    EHRegistrationNode *pRN,            // Registration node for subject
                                        //   function
    DispatcherContext *pDC,             // Context within subject frame
    FuncInfo *pFuncInfo,                // Static information for subject
                                        //   function
    __ehstate_t targetState             // State to unwind to
    )
{
    UNREFERENCED_PARAMETER(pDC); // This parameter is not used in some compilations

    EHTRACE_ENTER;

#if _EH_RELATIVE_FUNCINFO
    ptrdiff_t unwindImageBase = _GetImageBase();
#endif

    __ehstate_t curState = GetCurrentState(pRN, pDC, pFuncInfo);
    __ProcessingThrow++;
    __try {
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    while (curState != EH_EMPTY_STATE && curState > targetState)
#else
    while (curState != targetState)
#endif
    {
        DASSERT((curState > EH_EMPTY_STATE)
          && (curState < FUNC_MAXSTATE(*pFuncInfo)));

        // Get state after next unwind action
        __ehstate_t nxtState = UWE_TOSTATE(FUNC_UNWIND(*pFuncInfo, curState));

        __try {
            // Call the unwind action (if one exists):

            if (UWE_ACTION(FUNC_UNWIND(*pFuncInfo, curState)) != nullptr) {

                // Before calling unwind action, adjust state as if it were
                // already completed:
                SetState(pRN, pFuncInfo, nxtState);

                EHTRACE_FMT2("Unwind from state %d to state %d", curState, nxtState);
                RENAME_EH_EXTERN(_CallSettingFrame)(__GetAddress(UWE_ACTION(FUNC_UNWIND(*pFuncInfo, curState)),
                                                                 pDC),
                                                    REAL_FP(pRN, pFuncInfo),
#if defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                                    (PULONG)pDC->NonVolatileRegisters,
#endif
                                                    0x103);

#if _EH_RELATIVE_FUNCINFO
                _SetImageBase(unwindImageBase);
#endif
            }

        } __except(EHTRACE_EXCEPT(__FrameUnwindFilter(exception_info()))) {
            ; // Deliberately do nothing
        }

        curState = nxtState;
    }
    } __finally {
        if (__ProcessingThrow > 0) {
            __ProcessingThrow--;
        }
    }


    // Now that we're done, set the frame to reflect the final state.

#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    DASSERT(curState == EH_EMPTY_STATE || curState <= targetState);
#else
    DASSERT(curState == targetState);
#endif

    EHTRACE_FMT2("Move from state %d to state %d", GetCurrentState(pRN, pDC, pFuncInfo), curState);

    SetState(pRN, pFuncInfo, curState);

    EHTRACE_EXIT;
}

////////////////////////////////////////////////////////////////////////////////
//
// CatchIt - A handler has been found for the thrown type.  Do the work to
//   transfer control.
//
// Description:
//     Builds the catch object
//     Unwinds the stack to the point of the try
//     Calls the address of the handler (funclet) with the frame set up for that
//       function but without resetting the stack.
//     Handler funclet returns address to continue execution, or nullptr if the
//       handler re-threw ("throw;" lexically in handler)
//     If the handler throws an EH exception whose exception info is nullptr, then
//       it's a re-throw from a dynamicly enclosed scope.
//
// It is an open question whether the catch object is built before or after the local unwind.
//
// Returns:
//     No return value.  Returns iff handler re-throws.
template <class T>
static void CatchIt(
    EHExceptionRecord *pExcept,           // The exception thrown
    EHRegistrationNode *pRN,              // Dynamic info of function with catch
    CONTEXT *pContext,                    // Context info
    DispatcherContext *pDC,               // Context within subject frame
    typename T::FuncInfo *pFuncInfo,      // Static info of function with catch
    typename T::HandlerType *pCatch,      // The catch clause selected
    CatchableType *pConv,                 // The rules for making the conversion
    typename T::TryBlockMapEntry *pEntry, // Description of the try block
    int CatchDepth,                       // How many catches are we nested in?
    EHRegistrationNode *pMarkerRN,        // Special node if nested in catch
    BOOLEAN IsRethrow                     // Is this a rethrow ?
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    , BOOLEAN recursive
#endif // defined(_POWERPC)
) {

    // These parameters are not used in some compilations
    UNREFERENCED_PARAMETER(CatchDepth);
    UNREFERENCED_PARAMETER(pMarkerRN);
    UNREFERENCED_PARAMETER(IsRethrow);

    EHTRACE_ENTER_FMT1("Catching object @ 0x%p", PER_PEXCEPTOBJ(pExcept));

    EHRegistrationNode *pEstablisher = pRN;

#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    EHRegistrationNode EstablisherFramePointers;
    pEstablisher = T::GetEstablisherFrame(pRN, pDC, pFuncInfo, &EstablisherFramePointers);
#else
    void *continuationAddress;
#endif // defined(_POWERPC)

    // Copy the thrown object into a buffer in the handler's stack frame,
    // unless the catch was by elipsis (no conversion) OR the catch was by
    // type without an actual 'catch object'.

    if (pConv != nullptr) {
        T::BuildCatchObject(pExcept, pEstablisher, pCatch, pConv);
    }

    // Unwind stack objects to the entry of the try that caught this exception.

#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    // This call will never return. This call will end up calling CxxCallCatchBlock
    // through RtlUnwind (STATUS_CONSULIDATE_FRAMES) mechanism.
    T::UnwindNestedFrames(
        pRN,
        pExcept,
        pContext,
        pEstablisher,
        __GetAddress(HT_HANDLER(*pCatch), pDC),
        pFuncInfo,
        pEntry->tryLow,
        pEntry->catchHigh,
        pCatch,
        pDC,
        recursive
        );

#else

    if constexpr (std::is_same_v<T, RENAME_EH_EXTERN(__FrameHandler4)>)
    {
        // TODO: x86
        DASSERT(!"NYI");
    }
    else
    {
        if (pMarkerRN == nullptr) {
            RENAME_EH_EXTERN(_UnwindNestedFrames)(pRN, pExcept);
        }
        else {
            RENAME_EH_EXTERN(_UnwindNestedFrames)(pMarkerRN, pExcept);
        }


        T::FrameUnwindToState(pEstablisher, pDC, pFuncInfo, TBME_LOW(*pEntry));

        // Call the catch.  Separated out because it introduces a new registration
        // node.

        EHTRACE_FMT2("Move from state %d to state %d", T::GetCurrentState(pRN, pDC, pFuncInfo), TBME_HIGH(*pEntry) + 1);
        T::SetState(pRN, pFuncInfo, TBME_HIGH(*pEntry) + 1);

        continuationAddress = CallCatchBlock(pExcept,
            pEstablisher,
            pContext,
            pFuncInfo,
            __GetAddress(HT_HANDLER(*pCatch), pDC),
            CatchDepth,
            0x100);

        // Transfer control to the continuation address.  If no continuation then
        // it's a re-throw, so return.

        if (continuationAddress != nullptr) {

            _JumpToContinuation(continuationAddress, pRN);
            // No return.

        }
        EHTRACE_EXIT;
    }
#endif
}

#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_) /* { { */

static void __RethrowException(EHExceptionRecord *pThisException)
{
    RaiseException( pThisException->ExceptionCode,
                    pThisException->ExceptionFlags,
                    pThisException->NumberParameters,
                    (PULONG_PTR)&(pThisException->params) );
}

void * RENAME_EH_EXTERN(__FrameHandler4)::CxxCallCatchBlock(
    EXCEPTION_RECORD *pExcept
    )
{
    int rethrow = 0, TranslatedCatch = 0;
    void *handlerAddress;
    void *continuationAddress = nullptr;
    FRAMEINFO FrameInfo;
    CONTEXT *pContext, *pSaveContext;
    EHRegistrationNode *pEstablisherFrame;
    EHExceptionRecord *pThisException, *pSaveException, *pForeignException = nullptr;
    void *continuationAddresses[MAX_CONT_ADDRESSES];
    memset(continuationAddresses, 0, sizeof(continuationAddresses));
    FuncInfo4 FuncInfo;

    pSaveContext = _pCurrentExContext;
    pSaveException = _pCurrentException;

    // Copy Necessary Information which is passed from UnwindNestedFrames.
    pThisException = (EHExceptionRecord *)pExcept->ExceptionInformation[6];
    pContext = (CONTEXT *)pExcept->ExceptionInformation[4];
    handlerAddress = (void *)pExcept->ExceptionInformation[2];
    pEstablisherFrame = (EHRegistrationNode *)pExcept->ExceptionInformation[1];

    continuationAddresses[0] = (PBYTE)pExcept->ExceptionInformation[5];
    continuationAddresses[1] = (PBYTE)pExcept->ExceptionInformation[9];

    // State used if an exception inside the called catch needs to be handled in the parent
    __ehstate_t HandlerSearchState = (__ehstate_t) pExcept->ExceptionInformation[11];
    // State used during unwinding to bypass already unwound states in the parent
    __ehstate_t UnwindTryState = (__ehstate_t)pExcept->ExceptionInformation[3];

    __except_validate_context_record(pContext);

    _pCurrentException = pThisException;
    _pCurrentExContext = pContext;
    FRAMEINFO *pFrameInfo = RENAME_EH_EXTERN(_CreateFrameInfo)(&FrameInfo,
        (void *)PER_PEXCEPTOBJ(_pCurrentException));

    if (pExcept->ExceptionInformation[7]) {
        // Stuff for SehTranslation book keeping
        // This Exception Object is Translation of Foreign Exception And should
        // be destroyed in any case. If there is rethrow, throw ForeignException.
        // IMPORTANT: No one else can rethrow this Exception Object.
        TranslatedCatch = 1;
        pForeignException = _pForeignExcept;
        // End Translation Stuff
    }

    __try {
        __try {
            continuationAddress = RENAME_EH_EXTERN(_CallSettingFrame)(handlerAddress,
                pEstablisherFrame,
#if defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                (PULONG)pExcept->ExceptionInformation[10],
#endif
                0x100);

            intptr_t continuationIndex = reinterpret_cast<intptr_t>(continuationAddress);
            if (continuationIndex <= MAX_CONT_ADDRESSES)
            {
                DASSERT(continuationAddresses[continuationIndex] != 0);
                continuationAddress = continuationAddresses[continuationIndex];
            }
        }
        // Filter will be invoked in pass 1 looking for an exception, this lets us set up the correct _HandlerSearchState that should
        // be used when looking for a handler in the parent of a catch funclet
        __except (__vcrt_getptd()->_HandlerSearchState = HandlerSearchState, ExFilterRethrow(exception_info(), pThisException, &rethrow))
        {
            rethrow = 1;
            cxxReThrow = false;
            if (TranslatedCatch) {
                // Note in case of rethrow, no one else can convert rethrow to
                // pThisException. This means only this except will deal with conversion
                // of rethrow to pThisException. Instead of pThisException, we
                // will throw original Foreign Exception. Also we will need to
                // destroy Exception Object before Raising Foreign Exception.
                __DestructExceptionObject(pThisException, TRUE);
                __RethrowException(pForeignException);
            }
            else
            {
                __RethrowException(pThisException);
            }
        }
    }
    __finally {
        RENAME_EH_EXTERN(_FindAndUnlinkFrame)(pFrameInfo);
        if (!rethrow &&
            PER_IS_MSVC_EH(pThisException) &&
            _IsExceptionObjectToBeDestroyed(PER_PEXCEPTOBJ(pThisException)))
        {
            __DestructExceptionObject(pThisException, TRUE);
        }
        _pCurrentException = pSaveException;
        _pCurrentExContext = pSaveContext;

        // Finally is invoked in pass 2, letting us set the correct state to unwind from when back in the parent frame
        __vcrt_getptd()->_UnwindStateorOffset = UnwindTryState;
        __vcrt_getptd()->_IsOffset = false;
    }
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    __vcrt_getptd()->_UnwindStateorOffset = INVALID_CATCH_SPECIFIC_STATE; // non-exceptional path won't need this value
#endif
    return continuationAddress;
}

void * RENAME_EH_EXTERN(__FrameHandler3)::CxxCallCatchBlock(
    EXCEPTION_RECORD *pExcept
    )
{
    int rethrow = 0, TranslatedCatch = 0;
    void *handlerAddress;
    void *continuationAddress = nullptr;
    FuncInfo *pFuncInfo;
    FRAMEINFO FrameInfo;
    CONTEXT *pContext, *pSaveContext;
    EHRegistrationNode *pEstablisherFrame;
    EHExceptionRecord *pThisException, *pSaveException, *pForeignException = nullptr;

    pSaveContext = _pCurrentExContext;
    pSaveException = _pCurrentException;

    // Copy Necessary Information which is passed from UnwindNestedFrames.
    pThisException      = (EHExceptionRecord *) pExcept->ExceptionInformation[6];
    pFuncInfo           = (FuncInfo *)          pExcept->ExceptionInformation[5];
    pContext            = (CONTEXT *)           pExcept->ExceptionInformation[4];
    handlerAddress      = (void *)              pExcept->ExceptionInformation[2];
    pEstablisherFrame   = (EHRegistrationNode *)pExcept->ExceptionInformation[1];

    __except_validate_context_record(pContext);

    _pCurrentException  = pThisException;
    _pCurrentExContext = pContext;
    FRAMEINFO *pFrameInfo = RENAME_EH_EXTERN(_CreateFrameInfo)(&FrameInfo,
                                             (void *)PER_PEXCEPTOBJ(_pCurrentException));

    if (pExcept->ExceptionInformation[7]) {
        // Stuff for SehTranslation book keeping
        // This Exception Object is Translation of Foreign Exception And should
        // be destroyed in any case. If there is rethrow, throw ForeignException.
        // IMPORTANT: No one else can rethrow this Exception Object.
        TranslatedCatch = 1;
        pForeignException = _pForeignExcept;
        // End Translation Stuff
    }

    __try {
        __try{
            continuationAddress = RENAME_EH_EXTERN(_CallSettingFrame)(handlerAddress,
                                                                      pEstablisherFrame,
#if defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
                                                                      (PULONG)pExcept->ExceptionInformation[10],
#endif
                                                                      0x100);
        } __except(ExFilterRethrow(exception_info(),
                                   pThisException,
                                   &rethrow)) {
            rethrow = 1;
            cxxReThrow = false;
            if (TranslatedCatch) {
                // Note in case of rethrow, no one else can convert rethrow to
                // pThisException. This means only this except will deal with conversion
                // of rethrow to pThisException. Instead of pThisException, we
                // will throw original Foreign Exception. Also we will need to
                // destroy Exception Object before Raising Foreign Exception.
                __DestructExceptionObject(pThisException, TRUE);
                __RethrowException(pForeignException);
            } else {
                __RethrowException(pThisException);
            }
        }
    } __finally {
        RENAME_EH_EXTERN(_FindAndUnlinkFrame)(pFrameInfo);
        if ( !rethrow &&
             PER_IS_MSVC_EH(pThisException) &&
             _IsExceptionObjectToBeDestroyed(PER_PEXCEPTOBJ(pThisException)))
        {
            __DestructExceptionObject(pThisException, TRUE);
        }
        _pCurrentException = pSaveException;
        _pCurrentExContext = pSaveContext;
    }
#if defined(_M_X64) || defined(_M_ARM_NT) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    UNWINDHELP(*pEstablisherFrame, FUNC_DISPUNWINDHELP(*pFuncInfo)) = -2;
#endif
    return continuationAddress;
}

////////////////////////////////////////////////////////////////////////////////
//
// ExFilterRethrow - Exception filter for re-throw exceptions.
//
// Returns:
//     EXCEPTION_EXECUTE_HANDLER - exception was a re-throw
//     EXCEPTION_CONTINUE_SEARCH - anything else
//
// Side-effects: sets rethrow = TRUE if exception objects of the two Exception matches

static __declspec(guard(ignore)) int ExFilterRethrow(
    EXCEPTION_POINTERS *pExPtrs,
    EHExceptionRecord *pOldExcept,
    int *rethrow
) {
    // Get the exception record thrown (don't care about other info)
    EHExceptionRecord *pExcept = (EHExceptionRecord *)pExPtrs->ExceptionRecord;
    *rethrow = 0;
    if (PER_IS_MSVC_EH(pExcept) && PER_PEXCEPTOBJ(pExcept) == PER_PEXCEPTOBJ(pOldExcept))
        *rethrow = 1;
    // Check if it's ours and it's has no exception information.
    if (PER_IS_MSVC_EH(pExcept) && PER_PTHROW(pExcept) == nullptr) {
    // In a rethrow, ExceptionCode isn't set to EH_EXCEPTION_NUMBER even for a
    // C++ Exception. Hence we use cxxReThrow in __InternalCxxFrameHandler
    // to check for a C++ Exception
        cxxReThrow = true;
        *rethrow = 1;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(_M_IX86)

////////////////////////////////////////////////////////////////////////////////
//
// CallCatchBlock - continuation of CatchIt.
//
// This is separated from CatchIt because it needs to introduce an SEH/EH frame
//   in case the catch block throws.  This frame cannot be added until unwind of
//   nested frames has been completed (otherwise this frame would be the first
//   to go).

static __declspec(guard(ignore)) void *CallCatchBlock(
    EHExceptionRecord *pExcept,         // The exception thrown
    EHRegistrationNode *pRN,            // Dynamic info of function with catch
    CONTEXT *pContext,                  // Context info
    FuncInfo *pFuncInfo,                // Static info of function with catch
    void *handlerAddress,               // Code address of handler
    int CatchDepth,                     // How deeply nested in catch blocks
                                        //   are we?
    unsigned long NLGCode               // NLG destination code
) {
    EHTRACE_ENTER;

    // Address where execution resumes after exception handling completed.
    // Initialized to non-nullptr (value doesn't matter) to distinguish from
    // re-throw in __finally.
    void *continuationAddress = handlerAddress;

    BOOL ExceptionObjectDestroyed = FALSE;

    // The stack pointer at entry to the try must be saved, in case there is
    // another try inside this catch.  We'll restore it on our way out.
    void *saveESP = PRN_STACK(pRN);

    // Push this catch block's frame info on a linked list
    FRAMEINFO FrameInfo;
    FRAMEINFO *pFrameInfo = RENAME_EH_EXTERN(_CreateFrameInfo)(&FrameInfo, PER_PEXCEPTOBJ(pExcept));

    // Save the current exception in case of a rethrow.  Save the previous value
    // on the stack, to be restored when the catch exits.
    EHExceptionRecord *pSaveException = _pCurrentException;
    CONTEXT *pSaveExContext = _pCurrentExContext;

    _pCurrentException = pExcept;
    _pCurrentExContext = pContext;

    __try {
        __try {
            // Execute the handler as a funclet, whose return value is the
            // address to resume execution.
            continuationAddress = _CallCatchBlock2(pRN, pFuncInfo,
                handlerAddress, CatchDepth, NLGCode);
        } __except(EHTRACE_EXCEPT(ExFilterRethrow(exception_info()))) {
            cxxReThrow=false;
            // Here we are exiting the catch block on rethrow out of this
            // catch block. To keep the order of destruction and construction
            // same when the the rethrow was from function or was inline, here
            // we unwind to the parent state for this catch.
            UnwindMapEntry *pUnwindMap = pFuncInfo->pUnwindMap;
            int cState = RENAME_EH_EXTERN(__FrameHandler3)::GetCurrentState(pRN, handlerAddress, pFuncInfo);
            TryBlockMapEntry *pTryBlockMap = pFuncInfo->pTryBlockMap;
            unsigned int i;
            for (i = 0; i < pFuncInfo->nTryBlocks; i++) {
                if (cState > pTryBlockMap[i].tryHigh &&
                    cState <= pTryBlockMap[i].catchHigh) {
                    cState = pTryBlockMap[i].tryHigh +1;
                    cState = pUnwindMap[cState].toState;
                    break;
                }
            }
            RENAME_EH_EXTERN(__FrameHandler3)::FrameUnwindToState(pRN, nullptr, pFuncInfo, cState);
            // If the handler threw a typed exception without exception info or
            // exception object, then it's a re-throw, so return.  Otherwise
            // it's a new exception, which takes precedence over this one.
            continuationAddress = nullptr;
        }
    } __finally {
        EHTRACE_SAVE_LEVEL;
        EHTRACE_FMT1("Executing __finally, %snormal termination", _abnormal_termination() ? "ab" : "");

        // Restore the saved stack pointer, so the stack can be reset when
        // we're done.
        PRN_STACK(pRN) = saveESP;

        // Pop this catch block's frame info
        RENAME_EH_EXTERN(_FindAndUnlinkFrame)(pFrameInfo);

        // Restore the 'current exception' for a possibly enclosing catch
        _pCurrentException = pSaveException;
        _pCurrentExContext = pSaveExContext;

        // Destroy the original exception object if we're not exiting on a
        // re-throw and the object isn't also in use by a more deeply nested
        // catch.  Note that the catch handles destruction of its parameter.

        if (PER_IS_MSVC_EH(pExcept) && !ExceptionObjectDestroyed
            && continuationAddress != nullptr
            && _IsExceptionObjectToBeDestroyed(PER_PEXCEPTOBJ(pExcept)))
        {
            __DestructExceptionObject(pExcept, abnormal_termination() != FALSE);
        }

        EHTRACE_RESTORE_LEVEL(!!_abnormal_termination());
    }
    EHTRACE_EXIT;
    return continuationAddress;
}


////////////////////////////////////////////////////////////////////////////////
//
// ExFilterRethrow - Exception filter for re-throw exceptions.
//
// Returns:
//     EXCEPTION_EXECUTE_HANDLER - exception was a re-throw
//     EXCEPTION_CONTINUE_SEARCH - anything else
//
// Side-effects: NONE.

static int ExFilterRethrow(
    EXCEPTION_POINTERS *pExPtrs
) {
    // Get the exception record thrown (don't care about other info)
    EHExceptionRecord *pExcept = (EHExceptionRecord *)pExPtrs->ExceptionRecord;

    // Check if it's ours and it's has no exception information.
    if (PER_IS_MSVC_EH(pExcept) && PER_PTHROW(pExcept) == nullptr) {
    // In a rethrow, ExceptionCode isn't set to EH_EXCEPTION_NUMBER even for a
    // C++ Exception. Hence we use cxxReThrow in __InternalCxxFrameHandler
    // to check for a C++ Exception
        cxxReThrow = true;
        return EXCEPTION_EXECUTE_HANDLER;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

#endif /* } } */
#endif

////////////////////////////////////////////////////////////////////////////////
//
// BuildCatchObjectHelper - Copy or construct the catch object from the object thrown.
//
// Returns:
//     0 if nothing to be done for constructing object from caller side
//     1 if single parameter constructor is to be called.
//     2 if two parameter constructor is to be called.
//
// Side-effects:
//     A buffer in the subject function's frame is initialized.
//
// Open issues:
//     What happens if the constructor throws?  (or faults?)

template <class T>
static int BuildCatchObjectHelperInternal(
    EHExceptionRecord *pExcept, // Original exception thrown
    void *pRN,                  // This is a pointer to the object
                                // that we want to build while doing
                                // COM+ eh. If we are in our own eh,
                                // then this is a Registration node of
                                // catching function
    T *pCatch,                  // The catch clause that got it
    CatchableType *pConv        // The conversion to use
    )
{
    int retval = 0;
    EHTRACE_ENTER;

    // If the catch is by ellipsis, then there is no object to construct.
    // If the catch is by type(No Catch Object), then leave too!
    if (HT_IS_TYPE_ELLIPSIS(*pCatch) ||
        (!HT_DISPCATCH(*pCatch) && !HT_ISCOMPLUSEH(*pCatch))) {
        EHTRACE_EXIT;
        return 0;
    }

    void **pCatchBuffer;
    if (HT_ISCOMPLUSEH(*pCatch))
    {
        pCatchBuffer = (void **)pRN;
    }
    else
    {
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
        pCatchBuffer = (void **)OffsetToAddress(
                                HT_DISPCATCH(*pCatch),
                                *((EHRegistrationNode *)pRN)
                                );
#else
        pCatchBuffer = (void **)OffsetToAddress(
                                HT_DISPCATCH(*pCatch),
                                (EHRegistrationNode *)pRN
                                );
#endif
    }
    __try {
        if (HT_ISBADALLOCCOMPAT(*pCatch) && CT_ISSTDBADALLOC(*pConv) && __WinRTOutOfMemoryExceptionCallback)
        {
            void* pException = __WinRTOutOfMemoryExceptionCallback();

            if (_ValidateRead(pException)
              && _ValidateWrite(pCatchBuffer)) {
                *pCatchBuffer = pException;
                *pCatchBuffer = __AdjustPointer(*pCatchBuffer,
                  CT_THISDISP(*pConv));
            }
            else {
                terminate(); // Does not return; TKB
            }
        }
        else if (HT_ISREFERENCE(*pCatch)) {

            // The catch is of form 'reference to T'.  At the throw point we
            // treat both 'T' and 'reference to T' the same, i.e.
            // pExceptionObject is a (machine) pointer to T.  Adjust as
            // required.
            if (_ValidateRead(PER_PEXCEPTOBJ(pExcept))
              && _ValidateWrite(pCatchBuffer)) {
                *pCatchBuffer = PER_PEXCEPTOBJ(pExcept);
                *pCatchBuffer = __AdjustPointer(*pCatchBuffer,
                  CT_THISDISP(*pConv));
            }
            else {
                terminate(); // Does not return; TKB
            }
        }
        else if (CT_ISSIMPLETYPE(*pConv)) {

            // Object thrown is of simple type (this including pointers) copy
            // specified number of bytes.  Adjust the pointer as required.  If
            // the thing is not a pointer, then this should be safe since all
            // the entries in the THISDISP are 0.
            if (_ValidateRead(PER_PEXCEPTOBJ(pExcept))
              && _ValidateWrite(pCatchBuffer)) {
                memmove(pCatchBuffer, PER_PEXCEPTOBJ(pExcept), CT_SIZE(*pConv));

                if (CT_SIZE(*pConv) == sizeof(void*) && *pCatchBuffer != nullptr) {
                    *pCatchBuffer = __AdjustPointer(*pCatchBuffer,
                      CT_THISDISP(*pConv));
                }
            }
            else {
                terminate(); // Does not return; TKB
            }
        }
        else {

            // Object thrown is UDT.
            if (CT_COPYFUNC(*pConv) == nullptr) {

                // The UDT had a simple ctor.  Adjust in the thrown object,
                // then copy n bytes.
                if (_ValidateRead(PER_PEXCEPTOBJ(pExcept))
                  && _ValidateWrite(pCatchBuffer)) {
                    memmove(pCatchBuffer, __AdjustPointer(PER_PEXCEPTOBJ(pExcept),
                      CT_THISDISP(*pConv)), CT_SIZE(*pConv));
                }
                else {
                    terminate(); // Does not return; TKB
                }
            }
            else {

                // It's a UDT: make a copy using copy ctor

#pragma warning(disable:4191)

                if (_ValidateRead(PER_PEXCEPTOBJ(pExcept))
                  && _ValidateWrite(pCatchBuffer)
                  && _ValidateExecute((FARPROC)CT_COPYFUNC(*pConv))) {

#pragma warning(default:4191)

                    if (CT_HASVB(*pConv)) {
                        retval = 2;
                    }
                    else {
                        retval = 1;
                    }
                }
                else {
                    terminate(); // Does not return; TKB
                }
            }
        }
    } __except(EHTRACE_EXCEPT(EXCEPTION_EXECUTE_HANDLER)) {
        // Something went wrong when building the catch object.
        terminate();
    }

    EHTRACE_EXIT;
    return retval;
}

extern "C" _VCRTIMP int __cdecl RENAME_EH_EXTERN(__BuildCatchObjectHelper)(
    EHExceptionRecord *pExcept,         // Original exception thrown
    void *pRN,                          // This is a pointer to the object
                                        // that we want to build while doing
                                        // COM+ eh. If we are in our own eh,
                                        // then this is a Registration node of
                                        // catching function
    HandlerType *pCatch,                // The catch clause that got it
    CatchableType *pConv                // The conversion to use
    )
{
    return BuildCatchObjectHelperInternal<RENAME_EH_EXTERN(__FrameHandler3)::HandlerType>(pExcept, pRN, pCatch, pConv);
}

int RENAME_EH_EXTERN(__FrameHandler3)::BuildCatchObjectHelper(
    EHExceptionRecord *pExcept,
    void              *pRN,
    HandlerType       *pCatch,
    CatchableType     *pConv
    )
{
    return BuildCatchObjectHelperInternal<HandlerType>(pExcept, pRN, pCatch, pConv);
}

#if _EH_RELATIVE_FUNCINFO
int RENAME_EH_EXTERN(__FrameHandler4)::BuildCatchObjectHelper(
    EHExceptionRecord *pExcept,
    void              *pRN,
    HandlerType4      *pCatch,
    CatchableType     *pConv
    )
{
    return BuildCatchObjectHelperInternal<HandlerType4>(pExcept, pRN, pCatch, pConv);
}
#endif

////////////////////////////////////////////////////////////////////////////////
//
// BuildCatchObjectInternal - Copy or construct the catch object from the object thrown.
//
// Returns:
//     nothing.
//
// Side-effects:
//     A buffer in the subject function's frame is initialized.
//
// Open issues:
//     What happens if the constructor throws?  (or faults?)

template <class T>
static void BuildCatchObjectInternal(
    EHExceptionRecord *pExcept,         // Original exception thrown
    void *pRN,                          // This is a pointer to the object
                                        // that we want to build while doing
                                        // COM+ eh. If we are in our own eh,
                                        // then this is a Registration node of
                                        // catching function
    T *pCatch,                          // The catch clause that got it
    CatchableType *pConv                // The conversion to use
    )
{
    EHTRACE_ENTER;

    void **pCatchBuffer;
    if ( HT_ISCOMPLUSEH(*pCatch))
    {
        pCatchBuffer = (void **)pRN;
    }
    else
    {
#if defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
        pCatchBuffer = (void **)OffsetToAddress(
                                HT_DISPCATCH(*pCatch),
                                *((EHRegistrationNode *)pRN)
                                );
#else
        pCatchBuffer = (void **)OffsetToAddress(
                                HT_DISPCATCH(*pCatch),
                                (EHRegistrationNode *)pRN
                                );
#endif
    }

    __try {
        switch(BuildCatchObjectHelperInternal<T>(pExcept, pRN, pCatch, pConv))
        {
            case 1:
                _CallMemberFunction1((char *)pCatchBuffer,
                  CT_COPYFUNC(*pConv),
                  __AdjustPointer(PER_PEXCEPTOBJ(pExcept),
                  CT_THISDISP(*pConv)));
                break;
            case 2:
                _CallMemberFunction2((char *)pCatchBuffer,
                  CT_COPYFUNC(*pConv),
                  __AdjustPointer(PER_PEXCEPTOBJ(pExcept),
                  CT_THISDISP(*pConv)), 1);
                break;
            case 0:
                break;
            default:
                break;
        }
    } __except(EHTRACE_EXCEPT(EXCEPTION_EXECUTE_HANDLER)) {
        // Something went wrong when building the catch object.
        terminate();
    }

    EHTRACE_EXIT;
}

extern "C" _VCRTIMP void __cdecl RENAME_EH_EXTERN(__BuildCatchObject)(
    EHExceptionRecord *pExcept,         // Original exception thrown
    void *pRN,                          // This is a pointer to the object
                                        // that we want to build while doing
                                        // COM+ eh. If we are in our own eh,
                                        // then this is a Registration node of
                                        // catching function
    HandlerType *pCatch,                // The catch clause that got it
    CatchableType *pConv                // The conversion to use
    )
{
    BuildCatchObjectInternal<RENAME_EH_EXTERN(__FrameHandler3)::HandlerType>(pExcept, pRN, pCatch, pConv);
}

void RENAME_EH_EXTERN(__FrameHandler3)::BuildCatchObject(
    EHExceptionRecord *pExcept,
    void *pRN,
    HandlerType *pCatch,
    CatchableType *pConv
    )
{
    BuildCatchObjectInternal<HandlerType>(pExcept, pRN, pCatch, pConv);
}

#if _EH_RELATIVE_FUNCINFO
void RENAME_EH_EXTERN(__FrameHandler4)::BuildCatchObject(
    EHExceptionRecord *pExcept,
    void *pRN,
    HandlerType4 *pCatch,
    CatchableType *pConv
    )
{
    BuildCatchObjectInternal<HandlerType>(pExcept, pRN, pCatch, pConv);
}
#endif

//////////////////////////////////////////////////////////////////////////////////
// IsInExceptionSpec - Checks if the exception matches the exception specification
//                     list. Returns TRUE if it does, otherwise FALSE
//
static BOOLEAN IsInExceptionSpec(
    EHExceptionRecord *pExcept,         // Information for this (logical)
                                        //   exception
    ESTypeList *pESTypeList             // Static information for subject frame
)
{
    DASSERT( pESTypeList != nullptr );
    if(!pESTypeList)
    {
        // should never happen
        terminate();
    }

    BOOLEAN bFoundMatchingTypeInES = FALSE;

#if _EH_RELATIVE_TYPEINFO
    __int32 const *ppCatchable;
#elif defined(_WIN64)
    CatchableType * UNALIGNED const *ppCatchable;
#else
    CatchableType * const *ppCatchable;
#endif
    CatchableType *pCatchable;
    int catchables;

    // for every type in the exception spec...
    for (int i=0; i<EST_COUNT(pESTypeList); i++ )
    {
        // for all types that thrown object can be converted to...
        ppCatchable = THROW_CTLIST(*PER_PTHROW(pExcept));
        for (catchables = THROW_COUNT(*PER_PTHROW(pExcept));
            catchables > 0; catchables--, ppCatchable++) {

#if _EH_RELATIVE_TYPEINFO
            pCatchable = (CatchableType *)(_GetThrowImageBase() + *ppCatchable);
#else
            pCatchable = *ppCatchable;
#endif
            if (RENAME_EH_EXTERN(__TypeMatch)(EST_ARRAY(pESTypeList,i), pCatchable, PER_PTHROW(pExcept)))
            {
                bFoundMatchingTypeInES = TRUE;
                break;
            }
        }
    }

    return bFoundMatchingTypeInES;
}

//////////////////////////////////////////////////////////////////////////////////
// CallUnexpected - Calls unexpected and handles all exceptions
//                  thrown by it
//
// The unexpected() function shall not return, but it can throw (or re-throw) an
// exception.
//
//  If it throws a new exception which is allowed by the exception
//  specification which previously was violated, then
//  {
//      the search for another handler will continue at the call of the function
//      whose exception specification was violated.
//  }
//  else /* If it throws or rethrows an exception that the exception-specification does not
//      allow */ then the following happens:
//  {
//      If the exception-specification does not include the class std::bad_exception
//      (lib.bad.exception) then
//      {
//          the function terminate() is called,
//      }
//      otherwise
//      {
//          the thrown exception is replaced by an implementation-defined
//          object of the type std::bad_exception and the search for another handler
//          will continue at the call of the function whose exception-specification
//          was violated.
//      }
//  }
//
// Simple, isn't it?
//
#if 0
static void CallUnexpected( ESTypeList* pESTypeList )
{
    DASSERT( _pCurrentFuncInfo == nullptr );

    try
    {
        unexpected();
    }
    catch(...)
    {
        _pCurrentFuncInfo = pESTypeList;

        throw; // rethrow -- we will catch it in the FrameHandler
    }
    terminate();
}
#endif

//////////////////////////////////////////////////////////////////////////////////
// Is_bad_exception_allowed - checks if std::bad_exception belongs to the list
//
static BOOLEAN Is_bad_exception_allowed(ESTypeList *pESTypeList)
{
    for (int i=0; i<EST_COUNT(pESTypeList); i++ )
    {
        if( *(type_info*)HT_PTD(*EST_ARRAY(pESTypeList,i)) == typeid( std::bad_exception ) )
        {
            return TRUE;
        }
    }

    return FALSE;
}


