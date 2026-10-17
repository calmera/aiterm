// "Sign in to chronicle": a QR of GitHub's device page with the code pre-filled, the code in
// large type, and the flow's progress. UI task only.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void view_signin_start(void);    // starts a sign-in (needs Wi-Fi) and shows the view; errors go to the TUI
void view_signin_show(void);     // shows the view for a flow started elsewhere (the setup page)
void view_signin_refresh(void);  // re-reads the flow's status when visible
bool view_signin_visible(void);

#ifdef __cplusplus
}
#endif
