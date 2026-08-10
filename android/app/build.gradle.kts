plugins {
	alias(libs.plugins.android.application)
	alias(libs.plugins.kotlin.android)
}

android {
	namespace = "com.openbw.replays"
	compileSdk = 34

	// Pinned so CI and local builds compile the native code with the same
	// toolchain rather than whatever each machine happens to have installed.
	ndkVersion = "27.0.12077973"

	defaultConfig {
		applicationId = "com.openbw.replays"
		minSdk = 21
		targetSdk = 34
		versionCode = 1
		versionName = "0.1"

		externalNativeBuild {
			cmake {
				// openbw needs exceptions (its error() throws) and C++14.
				arguments += listOf("-DANDROID_STL=c++_shared")
				cppFlags += listOf("-fexceptions", "-frtti")
			}
		}

		ndk {
			// 32-bit ARM is included for older handsets; x86_64 keeps the
			// emulator usable for development.
			abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
		}
	}

	externalNativeBuild {
		cmake {
			path = file("src/main/cpp/CMakeLists.txt")
			version = "3.22.1"
		}
	}

	sourceSets {
		named("main") {
			// SDLActivity and friends ship as source with SDL rather than as an
			// artifact, so they are compiled straight out of the submodule.
			java.srcDir("../third_party/SDL/android-project/app/src/main/java")
		}
	}

	packaging {
		jniLibs {
			// SDLActivity dlopens libbwreplay.so from nativeLibraryDir, which
			// only holds the libraries if they are extracted at install time.
			useLegacyPackaging = true
		}
	}

	buildTypes {
		release {
			isMinifyEnabled = false
			proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}

	kotlinOptions {
		jvmTarget = "17"
	}

	buildFeatures {
		viewBinding = true
	}
}

dependencies {
	implementation(libs.androidx.appcompat)
	implementation(libs.androidx.activity)
	implementation(libs.androidx.recyclerview)
	implementation(libs.androidx.constraintlayout)
	implementation(libs.material)
}
