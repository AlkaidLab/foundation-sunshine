# Contributing
Read our contribution guide in our organization level
[docs](https://lizardbyte.readthedocs.io/en/latest/developers/contributing.html).

## Project Patterns

### Web UI
* The Web UI uses [Vite](https://vitejs.dev) as its build system.
* The HTML pages used by the Web UI are found in `./src_assets/common/assets/web`.
* [EJS](https://www.npmjs.com/package/vite-plugin-ejs) is used as a templating system for the pages
  (check `template_header.html` and `template_header_main.html`).
* The Style System is provided by [Bootstrap](https://getbootstrap.com).
* The JS framework used by the more interactive pages is [Vus.js](https://vuejs.org).

#### Building

@tabs{
  @tab{CMake | ```bash
    cmake -B build -G Ninja -S . --target web-ui
    ninja -C build web-ui
    ```}
  @tab{Manual | ```bash
    npm run dev
    ```}
}

### Localization
Sunshine and related LizardByte projects are being localized into various languages.
The default language is `en` (English).

![](https://app.lizardbyte.dev/uno/crowdin/LizardByte_graph.svg)

@admonition{Community | We are looking for language coordinators to help approve translations.
The goal is to have the bars above filled with green!
If you are interesting, please reach out to us on our Discord server.}

#### CrowdIn
The translations occur on [CrowdIn][crowdin-url].
Anyone is free to contribute to the localization there.

##### Translation Basics
* The brand names *LizardByte* and *Sunshine* should never be translated.
* Other brand names should never be translated. Examples include *AMD*, *Intel*, and *NVIDIA*.

##### CrowdIn Integration
How does it work?

When a change is made to Sunshine source code, a workflow generates new translation templates
that get pushed to CrowdIn automatically.

When translations are updated on CrowdIn, a push gets made to the *l10n_master* branch and a PR is made against the
*master* branch. Once the PR is merged, all updated translations are part of the project and will be included in the
next release.

#### Extraction

##### Web UI
Sunshine uses [Vue I18n](https://vue-i18n.intlify.dev) for localizing the UI.
The following is a simple example of how to use it.

* Add the string to the `./src_assets/common/assets/web/public/assets/locale/en.json` file, in English.
  ```json
  {
   "index": {
     "welcome": "Hello, Sunshine!"
   }
  }
  ```

  @note{The json keys should be sorted alphabetically. You can use the provided i18n tools to automatically
  format and sort all locale files: `npm run i18n:format`}

  @attention{Due to the integration with Crowdin, it is important to only add strings to the *en.json* file,
  and to not modify any other language files. After the PR is merged, the translations can take place
  on [CrowdIn][crowdin-url]. Once the translations are complete, a PR will be made
  to merge the translations into Sunshine.}

##### i18n Development Tools

The project provides several npm scripts to help maintain translation quality:

```bash
# Validate all locale files have the same keys as en.json
npm run i18n:validate

# Auto-sync missing keys to all locale files (uses English as placeholder)
npm run i18n:sync

# Format and sort all locale JSON files alphabetically
npm run i18n:format

# Check if files are properly formatted
npm run i18n:format:check

# Validate translations
npm run i18n:validate
```

**Workflow when adding new translation keys:**

1. Add new keys to `en.json` only
2. Run `npm run i18n:sync` to add missing keys to all locale files
3. Run `npm run i18n:format` to ensure consistent formatting
4. Run `npm run i18n:validate` to verify completeness
5. Commit your changes - CI will automatically validate the translations

The i18n validation is integrated into the CI pipeline and will prevent merging PRs with
incomplete or incorrectly formatted translation files.

* Use the string in the Vue component.
  ```html
  <template>
    <div>
      <!-- In template, use $t (global injection) -->
      <p>{{ $t('index.welcome') }}</p>
      
      <!-- Or use in attributes -->
      <input :placeholder="$t('index.placeholder')" />
      <button :title="$t('index.tooltip')">{{ $t('index.button') }}</button>
    </div>
  </template>
  
  <script setup>
  import { useI18n } from 'vue-i18n'
  
  // In script, use useI18n() to get t function
  const { t } = useI18n()
  
  const handleClick = () => {
    alert(t('index.success_message'))
    if (confirm(t('index.confirm_action'))) {
      // Handle confirmation
    }
  }
  </script>
  ```

  @tip{More formatting examples can be found in the
  [Vue I18n guide](https://vue-i18n.intlify.dev/guide/formatting.html).}

##### Internationalizing Existing Components

When internationalizing existing components with hardcoded text, follow these steps:

1. **Identify hardcoded strings** in the component (both in template and script)

2. **Add translation keys to `en.json`**:
   ```json
   {
     "mycomponent": {
       "title": "My Title",
       "button_text": "Click Me",
       "confirm_message": "Are you sure?"
     }
   }
   ```

3. **Replace hardcoded text in template**:
   ```vue
   <!-- Before -->
   <h1>我的标题</h1>
   <button>点击我</button>
   
   <!-- After -->
   <h1>{{ $t('mycomponent.title') }}</h1>
   <button>{{ $t('mycomponent.button_text') }}</button>
   ```

4. **Replace hardcoded text in script** (must use `useI18n()`):
   ```vue
   <script setup>
   import { useI18n } from 'vue-i18n'
   const { t } = useI18n()
   
   // Before
   const handleClick = () => {
     if (confirm('确定吗？')) {
       // ...
     }
   }
   
   // After
   const handleClick = () => {
     if (confirm(t('mycomponent.confirm_message'))) {
       // ...
     }
   }
   </script>
   ```

5. **Sync translation keys**:
   ```bash
   npm run i18n:sync
   npm run i18n:format
   npm run i18n:validate
   ```

@note{Always use `useI18n()` in `<script setup>` when you need translations in JavaScript code (like `alert()`, `confirm()`, etc.). The `$t` function is only available in templates through global injection.}

##### C++

There should be minimal cases where strings need to be extracted from C++ source code; however it may be necessary in
some situations. For example the system tray icon could be localized as it is user interfacing.

* Wrap the string to be extracted in a function as shown.
  ```cpp
  #include <boost/locale.hpp>
  #include <string>

  std::string msg = boost::locale::translate("Hello world!");
  ```

@tip{More examples can be found in the documentation for
[boost locale](https://www.boost.org/doc/libs/1_70_0/libs/locale/doc/html/messages_formatting.html).}

@warning{The below is for information only. Contributors should never include manually updated template files, or
manually compiled language files in Pull Requests.}

Strings are automatically extracted from the code to the `locale/sunshine.po` template file. The generated file is
used by CrowdIn to generate language specific template files. The file is generated using the
`.github/workflows/localize.yml` workflow and is run on any push event into the `master` branch. Jobs are only run if
any of the following paths are modified.

```yaml
- 'src/**'
```

When testing locally it may be desirable to manually extract, initialize, update, and compile strings. Python is
required for this, along with the python dependencies in the `./scripts/requirements.txt` file. Additionally,
[xgettext](https://www.gnu.org/software/gettext) must be installed.

* Extract, initialize, and update
  ```bash
  python ./scripts/_locale.py --extract --init --update
  ```

* Compile
  ```bash
  python ./scripts/_locale.py --compile
  ```

@attention{Due to the integration with CrowdIn, it is important to not include any extracted or compiled files in
Pull Requests. The files are automatically generated and updated by the workflow. Once the PR is merged, the
translations can take place on [CrowdIn][crowdin-url]. Once the translations are
complete, a PR will be made to merge the translations into Sunshine.}

### Testing

#### Clang Format
Source code is tested against the `.clang-format` file for linting errors. The workflow file responsible for clang
format testing is `.github/workflows/cpp-clang-format-lint.yml`.

Option 1:
```bash
find ./ -iname *.cpp -o -iname *.h -iname *.m -iname *.mm | xargs clang-format -i
```

Option 2 (will modify files):
```bash
python ./scripts/update_clang_format.py
```

#### Unit Testing
Sunshine uses [Google Test](https://github.com/google/googletest) for unit testing. Google Test is included in the
repo as a submodule. The test sources are located in the `./tests` directory.

The tests need to be compiled into an executable, and then run. The tests are built using the normal build process, but
can be disabled by setting the `BUILD_TESTS` CMake option to `OFF`.

To run the tests, execute the following command.

```bash
./build/tests/test_sunshine
```

To see all available options, run the tests with the `--help` flag.

```bash
./build/tests/test_sunshine --help
```

@tip{See the googletest [FAQ](https://google.github.io/googletest/faq.html) for more information on how to use
Google Test.}

We use [gcovr](https://www.gcovr.com) to generate code coverage reports,
and [Codecov](https://about.codecov.io) to analyze the reports for all PRs and commits.

Codecov will fail a PR if the total coverage is reduced too much, or if not enough of the diff is covered by tests.
In some cases, the code cannot be covered when running the tests inside of GitHub runners. For example, any test that
needs access to the GPU will not be able to run. In these cases, the coverage can be omitted by adding comments to the
code. See the [gcovr documentation](https://gcovr.com/en/stable/guide/exclusion-markers.html#exclusion-markers) for
more information.

Even if your changes cannot be covered in the CI, we still encourage you to write the tests for them. This will allow
maintainers to run the tests locally.

[crowdin-url]: https://translate.lizardbyte.dev

<div class="section_buttons">

| Previous                |                                                         Next |
|:------------------------|-------------------------------------------------------------:|
| [Building](building.md) | [Source Code](../third-party/doxyconfig/docs/source_code.md) |

</div>
