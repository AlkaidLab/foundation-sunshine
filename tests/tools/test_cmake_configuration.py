"""Build real miniature projects against the production CMake modules."""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True)
parser.add_argument('--cmake', required=True)
parser.add_argument('--compiler', required=True)
args, remaining = parser.parse_known_args()
SOURCE = pathlib.Path(args.source).resolve()


class Configuration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sunshine cmake ')
        self.root = pathlib.Path(self.temporary.name)
        self.project = self.root / 'source'
        self.build = self.root / 'build'
        self.project.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, content):
        path = self.project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')
        return path

    def run_command(self, command, success=True):
        result = subprocess.run(command, text=True, capture_output=True, timeout=90)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def configure(self, extra=(), generator='Ninja', success=True):
        return self.run_command([args.cmake, '-S', str(self.project), '-B', str(self.build),
                                 '-G', generator, *extra], success)

    def build_project(self, extra=(), success=True):
        return self.run_command([args.cmake, '--build', str(self.build), *extra], success)

    def include(self, name):
        return f'include("{(SOURCE / name).as_posix()}")\n'

    def launcher(self, name, code):
        script = self.write(name + '.py', code)
        if os.name == 'nt':
            return self.write(name + '.cmd', f'@"{sys.executable}" "{script}" %*\n')
        result = self.write(name, f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n')
        result.chmod(0o755)
        return result

    def test_debug_assertions_survive_both_generators(self):
        text = (SOURCE / 'cmake/targets/common.cmake').read_text(encoding='utf-8')
        self.write('debug.cmake', text[text.index('# Preserve the Windows debug'):])
        self.write('main.cpp', '#include <cstdio>\nint main() {\n#ifdef NDEBUG\nstd::puts("disabled");\n#else\nstd::puts("enabled");\n#endif\n}\n')
        self.write('src/nvhttp.cpp', '// Source-specific Debug flags must remain legal.\n')
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(DebugPolicy LANGUAGES CXX)\n'
                   + self.include('cmake/targets/project_options.cmake')
                   + 'add_executable(probe main.cpp)\ntarget_link_libraries(probe PRIVATE sunshine_project_options)\n'
                     'set(TEST_DIR "")\ninclude(debug.cmake)\n')
        for generator in ['Ninja', 'Ninja Multi-Config']:
            with self.subTest(generator=generator):
                self.build = self.root / generator.replace(' ', '-')
                extra = [f'-DCMAKE_CXX_COMPILER={args.compiler}']
                if generator == 'Ninja':
                    extra.append('-DCMAKE_BUILD_TYPE=Debug')
                self.configure(extra, generator)
                self.build_project(['--config', 'Debug'])
                exe = self.build / ('Debug' if generator != 'Ninja' else '') / ('probe.exe' if os.name == 'nt' else 'probe')
                self.assertEqual(self.run_command([str(exe)]).strip(), 'enabled')

    def test_presets_keep_parent_test_install_rules_intact(self):
        self.write('CMakePresets.json', (SOURCE / 'CMakePresets.json').read_text(encoding='utf-8'))
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(PresetDirectories NONE)\n'
                   'add_custom_target(parent)\nadd_subdirectory(tests)\n')
        self.write('tests/CMakeLists.txt', 'add_custom_target(child)\n')
        self.run_command([args.cmake, '-S', str(self.project), '--preset', 'dev-win'])
        install_rules = self.project / 'build/tests/cmake_install.cmake'
        before = install_rules.read_text()
        self.run_command([args.cmake, '-S', str(self.project), '--preset', 'test-win'])
        self.assertEqual(install_rules.read_text(), before)

    def test_source_manifest_keeps_application_and_test_variants_separate(self):
        module = SOURCE / 'src/confighttp'
        self.write('src/confighttp/sources.cmake', (module / 'sources.cmake').read_text(encoding='utf-8'))
        # Stand-ins keep this ownership check independent of HTTP dependencies.
        for path in module.iterdir():
            if path.suffix in ['.cpp', '.h']:
                self.write('src/confighttp/' + path.name, '// fixture\n')
        self.write('src/confighttp/auth.cpp', 'int variant() {\n#ifdef SUNSHINE_TESTS\nreturn 1;\n#else\nreturn 0;\n#endif\n}\n')
        self.write('app.cpp', '#include <cstdio>\nint variant();\nint main() { std::printf("%d", variant()); }\n')
        self.write('tests/main.cpp', (self.project / 'app.cpp').read_text())
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(SourceVariants LANGUAGES CXX)\n'
                   'include(src/confighttp/sources.cmake)\nadd_executable(application app.cpp)\n'
                   'target_sources(application PRIVATE ${SUNSHINE_CONFIGHTTP_SOURCES})\n'
                   'target_sources(application PRIVATE ${SUNSHINE_CONFIGHTTP_HEADERS})\n'
                   'add_subdirectory(tests)\n')
        self.write('tests/CMakeLists.txt', 'add_executable(test_variant main.cpp)\n'
                   'target_sources(test_variant PRIVATE ${SUNSHINE_CONFIGHTTP_SOURCES})\n'
                   'target_sources(test_variant PRIVATE ${SUNSHINE_CONFIGHTTP_HEADERS})\n'
                   'target_compile_definitions(test_variant PRIVATE SUNSHINE_TESTS)\n')
        self.configure([f'-DCMAKE_CXX_COMPILER={args.compiler}'])
        self.build_project()
        suffix = '.exe' if os.name == 'nt' else ''
        self.assertEqual(self.run_command([str(self.build / ('application' + suffix))]), '0')
        self.assertEqual(self.run_command([str(self.build / 'tests' / ('test_variant' + suffix))]), '1')

    def test_system_boost_path_defines_all_consumed_components(self):
        package = self.project / 'package'
        self.write('package/BoostConfigVersion.cmake', 'set(PACKAGE_VERSION 1.92.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nset(PACKAGE_VERSION_EXACT TRUE)\n')
        self.write('package/BoostConfig.cmake', 'add_library(Boost::headers INTERFACE IMPORTED)\n'
                   'foreach(component IN LISTS Boost_FIND_COMPONENTS)\n'
                   'add_library(Boost::${component} INTERFACE IMPORTED)\n'
                   'list(APPEND Boost_LIBRARIES Boost::${component})\nendforeach()\n')
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(SystemBoost NONE)\n'
                   + self.include('cmake/dependencies/Boost_Sunshine.cmake')
                   + 'foreach(component IN ITEMS asio beast crc format property_tree)\n'
                     'if(NOT TARGET Boost::${component})\nmessage(FATAL_ERROR "Missing ${component}")\nendif()\nendforeach()\n'
                     'if(FETCH_CONTENT_BOOST_USED)\nmessage(FATAL_ERROR "System package was not used")\nendif()\n')
        self.configure([f'-DBoost_DIR={package.as_posix()}', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'])

    def test_web_ui_is_incremental_and_recovers_missing_outputs(self):
        log = self.project / 'npm-calls.txt'
        npm = self.launcher('npm-stub', '''import json, os, pathlib, sys
root = pathlib.Path(__file__).parent
with (root / 'npm-calls.txt').open('a') as log: log.write(sys.argv[1] + '\\n')
if sys.argv[1] == 'ci': (root / 'node_modules').mkdir(exist_ok=True)
else:
    destination = pathlib.Path(sys.argv[sys.argv.index('--outDir') + 1])
    destination.mkdir(parents=True, exist_ok=True)
    for page in (root / 'src_assets/common/assets/web').glob('*.html'):
        (destination / page.name).write_text(page.read_text())
''')
        self.write('package.json', '{}\n')
        lock = self.write('package-lock.json', '{}\n')
        self.write('src_assets/common/assets/web/index.html', 'page\n')
        script = self.write('src_assets/common/assets/web/main.js', 'initial\n')
        self.write('scripts/vite/vite.config.js', '// config\n')
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(WebInputs NONE)\n'
                   f'set(NPM "{npm.as_posix()}")\nset(SUNSHINE_SOURCE_ASSETS_DIR "${{PROJECT_SOURCE_DIR}}/src_assets")\n'
                   'set(NPM_SOURCE_ASSETS_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}")\nset(NPM_ASSETS_DIR "${PROJECT_BINARY_DIR}")\n'
                   + self.include('cmake/targets/web_ui.cmake'))
        self.configure()
        self.build_project()
        self.assertEqual(log.read_text().splitlines(), ['ci', 'run'])
        self.build_project()
        self.assertEqual(log.read_text().splitlines(), ['ci', 'run'])
        script.write_text('edited\n')
        self.build_project()
        self.assertEqual(log.read_text().splitlines(), ['ci', 'run', 'run'])
        lock.write_text('{"changed":true}\n')
        self.build_project()
        self.assertEqual(log.read_text().splitlines(), ['ci', 'run', 'run', 'ci', 'run'])
        (self.build / 'assets/web/index.html').unlink()
        self.build_project()
        self.assertEqual(log.read_text().splitlines()[-1], 'run')
        self.assertEqual(log.read_text().splitlines().count('ci'), 2)
        shutil.rmtree(self.project / 'node_modules')
        self.build_project()
        self.assertEqual(log.read_text().splitlines()[-2:], ['ci', 'run'])

    def test_asset_copies_preserve_paths_edits_additions_and_removals(self):
        first = self.write('src_assets/common/assets/first/icon.txt', 'first\n')
        self.write('src_assets/common/assets/second/icon.txt', 'second\n')
        platform = 'windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux'
        shader = self.write(f'src_assets/{platform}/assets/shaders/example.txt', 'shader\n')
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(AssetInputs NONE)\n'
                   'add_custom_target(sunshine)\nset(SUNSHINE_SOURCE_ASSETS_DIR "${PROJECT_SOURCE_DIR}/src_assets")\n'
                   + self.include('cmake/packaging/assets.cmake'))
        self.configure()
        self.build_project()
        self.assertEqual((self.build / 'assets/first/icon.txt').read_text(), 'first\n')
        self.assertEqual((self.build / 'assets/second/icon.txt').read_text(), 'second\n')
        self.assertEqual((self.build / 'assets/shaders/example.txt').read_text(), 'shader\n')
        self.assertIn('no work to do', self.build_project())
        first.write_text('edited\n')
        self.build_project()
        self.assertEqual((self.build / 'assets/first/icon.txt').read_text(), 'edited\n')
        shader.write_text('edited shader\n')
        self.build_project()
        self.assertEqual((self.build / 'assets/shaders/example.txt').read_text(), 'edited shader\n')
        self.write('src_assets/common/assets/added.txt', 'added\n')
        self.build_project()
        self.assertTrue((self.build / 'assets/added.txt').is_file())
        first.unlink()
        self.build_project()
        self.assertFalse((self.build / 'assets/first/icon.txt').exists())
        self.write(f'src_assets/{platform}/assets/added.txt', 'duplicate\n')
        self.assertIn('Duplicate asset path', self.configure(success=False))

    def test_wayland_tracks_xml_and_reports_first_command_failure(self):
        scanner = self.launcher('scanner-stub', '''import pathlib, sys
root = pathlib.Path(__file__).parent
if (root / 'fail').exists() and sys.argv[1] == 'private-code': sys.exit(23)
pathlib.Path(sys.argv[3]).write_text(pathlib.Path(sys.argv[2]).read_text())
''')
        xml = self.write('protocols/unstable/example.xml', 'first\n')
        self.write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\nproject(WaylandInputs NONE)\n'
                   f'set(WAYLAND_SCANNER_EXECUTABLE "{scanner.as_posix()}")\n'
                   + self.include('cmake/macros/linux.cmake')
                   + 'GEN_WAYLAND("${PROJECT_SOURCE_DIR}/protocols" unstable example)\n'
                     'add_custom_target(protocol ALL DEPENDS ${PLATFORM_TARGET_FILES})\n')
        self.configure()
        self.assertFalse((self.build / 'generated-src/example.c').exists())
        self.build_project()
        xml.write_text('edited\n')
        self.build_project()
        self.assertEqual((self.build / 'generated-src/example.c').read_text(), 'edited\n')
        self.assertEqual((self.build / 'generated-src/example.h').read_text(), 'edited\n')
        self.write('fail', 'fail\n')
        xml.write_text('must fail\n')
        self.build_project(success=False)
        self.assertEqual((self.build / 'generated-src/example.h').read_text(), 'edited\n')



if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
