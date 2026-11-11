import { hasInjectionContext, inject } from 'vue'

/**
 * Platform variants of a message (`key_windows`, `key_unix`), total: nothing here throws, whatever is or is not provided.
 *
 * The platform comes from the page that provides it (the settings page does), else from the last platform the host reported
 * (`setPlatformHint`, set by whoever reads the host's config), else from none: then the plain key and the unix variant are tried.
 * The message catalog comes from the page's injection, else from the one `init.js` registered (`setPlatformI18n`), else there is
 * none and the answer is the default message or the key. An async callback has no injection context, so the module-level values
 * are what it uses; before this, the home page's `$tp` dereferenced an absent injection and a session on the home page broke it
 * (TypeError: Cannot read properties of undefined (reading 'value')).
 */
let registeredI18n = null
let platformHint = ''

/** The message catalog (`i18n.global`) the app was created with; the fallback for a call outside of a component. */
export function setPlatformI18n(i18n) {
    registeredI18n = i18n && typeof i18n.t === 'function' ? i18n : null
}

/** The platform the host reported ('windows', 'linux', 'macos'); the fallback for a page that does not provide one. */
export function setPlatformHint(platform) {
    platformHint = typeof platform === 'string' ? platform : ''
}

function injected(name) {
    if (!hasInjectionContext()) return undefined
    try {
        return inject(name, undefined)
    } catch {
        return undefined
    }
}

function platformOf(platform) {
    const given = platform && typeof platform === 'object' && 'value' in platform ? platform.value : platform
    if (typeof given === 'string' && given) return given
    const provided = injected('platform')
    const value = provided && typeof provided === 'object' && 'value' in provided ? provided.value : provided
    return typeof value === 'string' && value ? value : platformHint
}

function catalogOf() {
    const provided = injected('i18n')
    return provided && typeof provided.t === 'function' ? provided : registeredI18n
}

class PlatformMessageI18n {
    /**
     * @param {string} platform
     */
    constructor(platform) {
        this.platform = platform || ''
    }

    /**
     * @param {string} key
     * @param {string} platform identifier
     * @return {string} key with platform identifier
     */
    getPlatformKey(key, platform) {
        return key + '_' + platform
    }

    /**
     * @param {string} key
     * @param {string?} defaultMsg
     * @return {string} translated message, the unix variant, defaultMsg if provided, or the key; never throws
     */
    getMessageUsingPlatform(key, defaultMsg) {
        const i18n = catalogOf()
        const fallback = defaultMsg ? defaultMsg : key
        if (!i18n) return fallback
        const lookup = (name) => {
            try {
                const message = i18n.t(name)
                return typeof message === 'string' && message !== name ? message : null
            } catch {
                return null
            }
        }

        const platform = this.platform || platformOf()
        if (platform) {
            const own = lookup(this.getPlatformKey(key, platform))
            if (own !== null) return own
            // If on Windows, we don't fall back to unix
            if (platform === 'windows') return fallback
        }

        // there's no message for the platform (or the platform is not known yet), check the unix version, then the plain key
        const unix = lookup(this.getPlatformKey(key, 'unix'))
        if (unix !== null) return unix
        const plain = platform ? null : lookup(key)
        return plain !== null ? plain : fallback
    }
}

/**
 * @param {string?} platform
 * @return {PlatformMessageI18n} instance; an unknown platform gives an instance that resolves it when it is asked
 */
export function usePlatformI18n(platform) {
    const resolved = platformOf(platform)
    const shared = injected('platformMessage')
    if (shared && typeof shared.getMessageUsingPlatform === 'function') return shared
    return new PlatformMessageI18n(resolved)
}

/**
 * @param {string} key
 * @param {string?} defaultMsg
 * @return {string} translated message or defaultMsg if provided
 */
export function $tp(key, defaultMsg) {
    try {
        return usePlatformI18n().getMessageUsingPlatform(key, defaultMsg)
    } catch {
        return defaultMsg ? defaultMsg : String(key)
    }
}
