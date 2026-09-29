#include <android_native_app_glue.h>

#include <stdlib.h>

// The glue's entry point, handing over to the app's ordinary main(). C rather
// than C++ because only C may call main.

int main(int argc, char* argv[]);

void eacpAndroidStart(struct android_app* app);
void eacpAndroidFinish(struct android_app* app);

void android_main(struct android_app* app)
{
    char name[] = "eacp";
    char* argv[] = {name, NULL};

    eacpAndroidStart(app);
    int result = main(1, argv);
    eacpAndroidFinish(app);

    // Static state (the loop, the app singletons) is not built to run twice,
    // and a recreated activity in this process would call android_main again.
    exit(result);
}
