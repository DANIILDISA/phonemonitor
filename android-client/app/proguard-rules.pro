# =============================================================================
#  Правила ProGuard/R8 для PhoneMonitor Client.
# =============================================================================
#
#  Минификация в релизной сборке отключена (см. app/build.gradle.kts), но
#  правила описаны: они понадобятся, если её когда-нибудь включат.

# MediaCodec обращается к колбэкам через JNI по именам методов.
-keep class android.media.MediaCodec$Callback { *; }

# Классы протокола содержат data-классы, чьи поля читаются рефлексией
# только в отладочных целях, но сохраняем их имена для читаемых стектрейсов.
-keepnames class com.phonemonitor.client.Protocol$* { *; }

# Читаемые номера строк в отчётах об ошибках.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
