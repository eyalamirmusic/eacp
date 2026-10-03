#include <android_native_app_glue.h>

#include <stdlib.h>

// The glue's entry point, handing over to the app's ordinary main(). C rather
// than C++ because only C may call main.

int main(int argc, char* argv[]);

void eacpAndroidStart(struct android_app* app);
int eacpAndroidFinish(struct android_app* app);

// Once per activity, on a thread of the glue's. Android destroys and recreates
// an activity in the same process for a configuration change the manifest does
// not claim (the font size, the locale) and when it reclaims a stopped one, and
// the recreated activity calls this again: the loop, the device and the app's
// own statics all run a second time, so main() must be written to run twice.
// Only an app whose main() returned on its own ends the process.
void android_main(struct android_app* app)
{
    char name[] = "eacp";
    char* argv[] = {name, NULL};

    eacpAndroidStart(app);
    int result = main(1, argv);

    if (eacpAndroidFinish(app))
        return;

    exit(result);
}
