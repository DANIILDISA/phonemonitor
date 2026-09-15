// =============================================================================
//  Настройки Gradle-сборки Android-клиента PhoneMonitor.
// =============================================================================
//
//  Репозитории объявлены здесь централизованно (FAIL_ON_PROJECT_REPOS):
//  так исключается ситуация, когда отдельный модуль тянет зависимости из
//  непроверенного источника.

pluginManagement {
    repositories {
        google {
            content {
                // Ограничиваем группы, которые берём из Google Maven:
                // это заметно ускоряет разрешение зависимостей.
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "PhoneMonitor"
include(":app")
