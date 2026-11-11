-keep class dev.cyclone.padtool.NativeCore { *; }
-keep class dev.cyclone.padtool.NativeUsb { *; }
# The page calls these by name (window.JollyMaamAndroid).
-keepclassmembers class dev.cyclone.padtool.Bridge { @android.webkit.JavascriptInterface <methods>; }
