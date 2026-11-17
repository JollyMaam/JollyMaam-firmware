-keep class dev.jollymaam.app.NativeCore { *; }
-keep class dev.jollymaam.app.NativeUsb { *; }
# The page calls these by name (window.JollyMaamAndroid).
-keepclassmembers class dev.jollymaam.app.Bridge { @android.webkit.JavascriptInterface <methods>; }
