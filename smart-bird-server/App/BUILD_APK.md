# BirdLink Android APK 打包说明

当前目录已经是 Capacitor Android App 工程，不是只有网页文件。

## 需要先安装

1. Android Studio
2. Android SDK Platform
3. Android SDK Build-Tools
4. JDK 11 或 Android Studio 自带的 JBR

当前电脑只有 Java 8，并且没有 Android SDK，所以还不能在本机直接编译出 APK。
下面路径里的 `BIRD_PROJECT_ROOT` 指向本项目根目录，可以通过系统环境变量配置。
项目依赖缓存默认放在 `%BIRD_PROJECT_ROOT%\.deps`，其中 Gradle 使用 `%BIRD_PROJECT_ROOT%\.deps\gradle`，npm 使用 `%BIRD_PROJECT_ROOT%\.deps\npm-cache`。

## 打包步骤

在 Android Studio 中打开：

```text
%BIRD_PROJECT_ROOT%\App\android
```

如果用命令行构建，先加载项目内依赖环境：

```powershell
. "%BIRD_PROJECT_ROOT%\use_project_deps.ps1"
cd "%BIRD_PROJECT_ROOT%\App\android"
.\gradlew.bat assembleDebug
```

等待 Gradle 同步完成后：

```text
Build > Build Bundle(s) / APK(s) > Build APK(s)
```

生成的 debug APK 通常在：

```text
%BIRD_PROJECT_ROOT%\App\android\app\build\outputs\apk\debug\app-debug.apk
```

这个 `.apk` 就可以发给安卓手机安装。

## 修改 App 后同步到 Android

如果改了 `index.html`、`styles.css` 或 `app.js`，先执行：

```powershell
. "%BIRD_PROJECT_ROOT%\use_project_deps.ps1"
cd "%BIRD_PROJECT_ROOT%\App"
npm run web:copy
npm run android:sync
```

再用 Android Studio 重新 Build APK。
