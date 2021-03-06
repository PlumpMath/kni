package org.jetbrains.kni.tests

import org.jetbrains.kni.gen.Printer
import java.io.File
import java.util.*

object GenerateTests {
    private fun generate(rootDir: String, testName: String, baseClass: String) {
        val output = File("tests/src/generated/${testName}Generated.kt")
        val mutedTests = findMutedTests(output)

        val sb = StringBuilder()
        val out = Printer(sb)

        out.println("// This file is auto-generated. You can mute individual tests by commenting them:")
        out.println("// '// @Test fun ...'. All other changes will be lost!")
        out.println()
        out.println("package org.jetbrains.kni.tests")
        out.println()
        out.println("import org.junit.Test")
        out.println()
        out.println("class ${testName}Generated : $baseClass() {")
        out.push()

        val root = File(rootDir)
        root.walkTopDown().forEach { file ->
            if (file.isFile && file.path.endsWith(".kt")) {
                val name = root.relativePath(file).substringBeforeLast(".kt").replace(File.separatorChar, '_')
                val mute = if (name in mutedTests) "// " else ""
                out.println("$mute@Test fun $name() = doTest(\"$file\")")
            }
        }

        out.pop()
        out.println("}")

        output.parentFile.mkdirs()
        output.writeText(sb.toString())
    }

    private fun findMutedTests(file: File): Set<String> {
        if (!file.exists()) return setOf()

        val result = HashSet<String>()
        file.forEachLine { line ->
            val rest = line.trim().substringAfter("// @Test fun ", "")
            if (rest != "") {
                result.add(rest.substringBefore('('))
            }
        }
        return result
    }

    @JvmStatic fun main(args: Array<String>) {
        generate("tests/testData/integration", "IntegrationTest", "ObjCGeneratedTest")
        generate("tests/testData/integration_cpp", "CPPIntegrationTest", "CPlusPlusGeneratedTest")
    }
}
