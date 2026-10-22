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

    def cmake_project(self, body, language='NONE'):
        self.write('CMakeLists.txt', f'cmake_minimum_required(VERSION 3.25)\nproject(Regression LANGUAGES {language})\n' + body)

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
        self.write('src/nvhttp.cpp', '#ifdef NDEBUG\n#error Debug assertions disabled\n#endif\nint main() {}\n')
        self.cmake_project(self.include('cmake/targets/project_options.cmake')
                           + 'add_executable(probe src/nvhttp.cpp)\n'
                             'target_link_libraries(probe PRIVATE sunshine_project_options)\n'
                             'sunshine_configure_debug_sources("")\n', 'CXX')
        for generator in ['Ninja', 'Ninja Multi-Config']:
            with self.subTest(generator=generator):
                self.build = self.root / generator.replace(' ', '-')
                build_type = 'Debug' if generator == 'Ninja' else ''
                self.configure([f'-DCMAKE_CXX_COMPILER={args.compiler}', f'-DCMAKE_BUILD_TYPE={build_type}'], generator)
                self.build_project(['--config', 'Debug'])

    def test_presets_keep_parent_test_install_rules_intact(self):
        self.write('CMakePresets.json', (SOURCE / 'CMakePresets.json').read_text(encoding='utf-8'))
        self.cmake_project('add_subdirectory(tests)\n')
        self.write('tests/CMakeLists.txt', 'add_custom_target(child)\n')
        self.run_command([args.cmake, '-S', str(self.project), '--preset', 'dev-win'])
        install_rules = self.project / 'build/tests/cmake_install.cmake'
        before = install_rules.read_text()
        self.run_command([args.cmake, '-S', str(self.project), '--preset', 'test-win'])
        self.assertEqual(install_rules.read_text(), before)

    def test_system_boost_path_defines_all_consumed_components(self):
        package = self.project / 'package'
        self.write('package/BoostConfigVersion.cmake', 'set(PACKAGE_VERSION 1.92.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nset(PACKAGE_VERSION_EXACT TRUE)\n')
        self.cmake_project(self.include('cmake/dependencies/Boost_Sunshine.cmake')
                   + 'foreach(component IN ITEMS boost ${BOOST_COMPONENTS})\n'
                     'if(NOT TARGET Boost::${component})\nmessage(FATAL_ERROR "Missing ${component}")\nendif()\nendforeach()\n'
                     'if(FETCH_CONTENT_BOOST_USED)\nmessage(FATAL_ERROR "Unexpected fallback")\nendif()\n')
        # Real system packages export the modern headers target or legacy boost target.
        for headers in ['headers', 'boost']:
            with self.subTest(headers=headers):
                self.build = self.root / f'build-{headers}'
                self.write('package/BoostConfig.cmake', f'add_library(Boost::{headers} INTERFACE IMPORTED)\n'
                           'foreach(component IN LISTS Boost_FIND_COMPONENTS)\n'
                           'add_library(Boost::${component} INTERFACE IMPORTED)\n'
                           'endforeach()\n')
                self.configure([f'-DBoost_DIR={package.as_posix()}', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'])

    def test_stale_boost_redirect_restores_cached_targets_on_reconfigure(self):
        package = self.project / 'stale-package'
        self.write('stale-package/BoostConfigVersion.cmake', 'set(PACKAGE_VERSION 1.92.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nset(PACKAGE_VERSION_EXACT TRUE)\n')
        self.write('stale-package/BoostConfig.cmake', 'set(Boost_FOUND TRUE)\n')
        cached = self.write('cached-boost/CMakeLists.txt',
                            'add_library(Boost::headers INTERFACE IMPORTED GLOBAL)\n'
                            'foreach(component IN LISTS BOOST_INCLUDE_LIBRARIES)\n'
                            'add_library(Boost::${component} INTERFACE IMPORTED GLOBAL)\n'
                            'endforeach()\n').parent
        self.cmake_project(self.include('cmake/dependencies/Boost_Sunshine.cmake')
                   + 'if(NOT FETCH_CONTENT_BOOST_USED)\nmessage(FATAL_ERROR "Stale package was accepted")\nendif()\n'
                     'foreach(component IN ITEMS headers boost ${BOOST_COMPONENTS})\n'
                     'if(NOT TARGET Boost::${component})\nmessage(FATAL_ERROR "Missing ${component}")\nendif()\nendforeach()\n'
                     'get_target_property(headers_link sunshine_boost_headers INTERFACE_LINK_LIBRARIES)\n'
                     'if(NOT headers_link STREQUAL "Boost::headers")\nmessage(FATAL_ERROR "Broken headers compatibility target")\nendif()\n')
        options = [f'-DBoost_DIR={package.as_posix()}', f'-DFETCHCONTENT_SOURCE_DIR_BOOST={cached.as_posix()}',
                   '-DFETCHCONTENT_FULLY_DISCONNECTED=ON']
        self.configure(options)
        # FetchContent creates its own package redirect; the next configure must restore targets again.
        self.configure(options)

    def test_web_ui_is_incremental_and_recovers_missing_outputs(self):
        log = self.project / 'npm-calls.txt'
        npm = self.launcher('npm-stub', '''import pathlib, sys
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
        self.cmake_project(f'set(NPM "{npm.as_posix()}")\nset(SUNSHINE_SOURCE_ASSETS_DIR "${{PROJECT_SOURCE_DIR}}/src_assets")\n'
                   'set(NPM_SOURCE_ASSETS_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}")\nset(NPM_ASSETS_DIR "${PROJECT_BINARY_DIR}")\n'
                   + self.include('cmake/targets/web_ui.cmake'))
        self.configure()
        def calls():
            before = len(log.read_text().splitlines()) if log.exists() else 0
            self.build_project()
            return log.read_text().splitlines()[before:]
        self.assertEqual(calls(), ['ci', 'run'])
        self.assertEqual(calls(), [])
        script.write_text('edited\n')
        self.assertEqual(calls(), ['run'])
        lock.write_text('{"changed":true}\n')
        self.assertEqual(calls(), ['ci', 'run'])
        (self.build / 'assets/web/index.html').unlink()
        self.assertEqual(calls(), ['run'])
        shutil.rmtree(self.project / 'node_modules')
        self.assertEqual(calls(), ['ci', 'run'])

    def test_asset_copies_preserve_paths_edits_additions_and_removals(self):
        first = self.write('src_assets/common/assets/first/icon.txt', 'first\n')
        self.write('src_assets/common/assets/second/icon.txt', 'second\n')
        platform = 'windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux'
        shader = self.write(f'src_assets/{platform}/assets/shaders/example.txt', 'shader\n')
        self.cmake_project('add_custom_target(sunshine)\nset(SUNSHINE_SOURCE_ASSETS_DIR "${PROJECT_SOURCE_DIR}/src_assets")\n'
                   + self.include('cmake/packaging/assets.cmake'))
        self.configure()
        self.build_project()
        self.assertEqual((self.build / 'assets/first/icon.txt').read_text(), 'first\n')
        self.assertEqual((self.build / 'assets/second/icon.txt').read_text(), 'second\n')
        self.assertEqual((self.build / 'assets/shaders/example.txt').read_text(), 'shader\n')
        self.assertIn('no work to do', self.build_project())
        first.write_text('edited\n')
        shader.write_text('edited shader\n')
        self.build_project()
        self.assertEqual((self.build / 'assets/first/icon.txt').read_text(), 'edited\n')
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
        self.cmake_project(f'set(WAYLAND_SCANNER_EXECUTABLE "{scanner.as_posix()}")\n'
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
