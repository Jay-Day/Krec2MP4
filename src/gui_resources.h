#pragma once

// Control IDs
#define IDC_ROM_PATH        101
#define IDC_ROM_BROWSE      102
#define IDC_INPUT_PATH      103
#define IDC_INPUT_BROWSE    104
#define IDC_BATCH_CHECK     105
#define IDC_OUTPUT_PATH     106
#define IDC_OUTPUT_BROWSE   107

#define IDC_RESOLUTION      201
#define IDC_QUALITY         202
#define IDC_FPS_EDIT        204
#define IDC_MSAA_SLIDER     205
#define IDC_MSAA_VALUE      206
#define IDC_ANISO_SLIDER    207
#define IDC_ANISO_VALUE     208
#define IDC_ENCODER         209

#define IDC_VERBOSE_CHECK   401
#define IDC_CONVERT_BTN     402
#define IDC_CANCEL_BTN      403
#define IDC_OPEN_FOLDER_BTN 404

#define IDC_PROGRESS_BAR    501
#define IDC_PROGRESS_TEXT   502
#define IDC_LOG_EDIT        503

// Custom window messages
#define WM_APP_LOG          (WM_APP + 1)  // wParam=level, lParam=_strdup'd string
#define WM_APP_PROGRESS     (WM_APP + 2)  // wParam=current_frame, lParam=total_frames
#define WM_APP_DONE         (WM_APP + 3)  // wParam=success_count, lParam=fail_count
#define WM_APP_BATCH        (WM_APP + 4)  // wParam=current_file (1-based), lParam=total_files
