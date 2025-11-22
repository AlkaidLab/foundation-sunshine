<template>
</template>

<script>
import { createI18n } from "vue-i18n";

// Import translation files
import de from '../../public/assets/locale/de.json'
import en from '../../public/assets/locale/en.json'
import en_GB from '../../public/assets/locale/en_GB.json'
import en_US from '../../public/assets/locale/en_US.json'
import es from '../../public/assets/locale/es.json'
import fr from '../../public/assets/locale/fr.json'
import it from '../../public/assets/locale/it.json'
import ru from '../../public/assets/locale/ru.json'
import sv from '../../public/assets/locale/sv.json'
import zh from '../../public/assets/locale/zh.json'

// Create the i18n instance
const i18n = createI18n({
    locale: "en",  // initial locale, todo: how to get this from config?
    fallbackLocale: "en",  // fallback locale
    messages: {
        de: de,
        en: en,
        "en-GB": en_GB,
        "en-US": en_US,
        es: es,
        fr: fr,
        it: it,
        ru: ru,
        sv: sv,
        zh: zh
    },
});

export {
    i18n
};

export default {
    created() {
        this.fetchLocale();
    },
    methods: {
        fetchLocale() {
            fetch("/api/configLocale$")
                .then((r) => r.json())
                .then((r) => {
                    this.response = r;
                    i18n.locale = this.response.locale;
                });
        },
    },
};
</script>
