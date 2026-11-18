; Keep program files out of %LOCALAPPDATA%\ZEND (the default currentUser target).
; The loader and core write logs, session, queue, and workspace data to that
; path; NTFS treats it as the same directory. Install under Programs\ instead.
!macro NSIS_HOOK_PREINSTALL
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${PRODUCTNAME}"
  SetOutPath "$INSTDIR"
!macroend

; Tauri keeps the previous install location under this key for reinstalls.
; Once the app is gone there is nothing to restore, so drop it.
!macro NSIS_HOOK_POSTUNINSTALL
  DeleteRegKey SHCTX "${MANUPRODUCTKEY}"
  DeleteRegKey /ifempty SHCTX "${MANUKEY}"
!macroend
