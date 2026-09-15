// Корневой build-файл. Плагины объявляются здесь без применения (apply false),
// а подключаются уже в модулях - это стандартная схема для многомодульных
// проектов и она не мешает текущему однмодульному варианту.
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
