# SDLActivity and its helpers are reached from native code by name, so their
# entry points must survive shrinking.
-keep class org.libsdl.app.** { *; }

# NativeBridge's external functions are bound by JNI name.
-keep class com.openbw.replays.NativeBridge { *; }
