import java.io.File
import java.util.Properties

// The Pkg.Revision of the NDK at path, null if there is none.
fun ndkRevision(path: String): String? {
    val props = File(path, "source.properties")
    if (!props.exists())
        return null
    return Properties().apply { props.inputStream().use { load(it) } }
        .getProperty("Pkg.Revision")
}

plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "se.sanctorum.vigil"
    compileSdk = flutter.compileSdkVersion
    // Must be the same NDK dist-build/android-build.sh used, see
    // dist-build/android-sdk.sh for how it is found.
    val pinnedNdk = "28.2.13676358"
    val fetchedNdk = "${rootDir.parentFile.parentFile}/build/tools/android-ndk"
    val standaloneNdk = (System.getenv("ANDROID_NDK_HOME")
        ?: System.getenv("ANDROID_NDK_ROOT"))?.takeIf { it.isNotEmpty() }
        ?: fetchedNdk.takeIf { ndkRevision(it) == pinnedNdk }

    if (standaloneNdk != null) {
        ndkPath = standaloneNdk
        ndkVersion = ndkRevision(standaloneNdk)
            ?: throw GradleException("$standaloneNdk is not an android ndk")
    } else {
        ndkVersion = pinnedNdk
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "se.sanctorum.vigil"
        // libnyfe uses getrandom(2), which is only reliable from API 28 on.
        minSdk = 28
        targetSdk = flutter.targetSdkVersion
        // Uses the version code from pubspec.yaml. When using split APKs, 1000 * ABI_VERSION
        // is added automatically by Flutter. (https://developer.android.com/studio/build/configure-apk-splits#configure-APK-versions)
        // You can force using the value of versionCode by specifying the `-P force-version-code-ignoring-abi=true`
        // flag during build.
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DVIGIL_ROOT=${rootDir.parentFile.parentFile}")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
