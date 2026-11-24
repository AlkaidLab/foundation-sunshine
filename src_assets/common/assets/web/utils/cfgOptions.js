// Option lists of the configuration rows: [{ value, label }] with labels from the locale.

/** One option per value with the label `<prefix>_<value>` ('config.qsv_preset', ['fast']) -> config.qsv_preset_fast. */
export const localeOptions = (t, prefix, values) => values.map((value) => ({ value: String(value), label: t(`${prefix}_${value}`) }))

/** Options with explicit locale keys: [['auto', 'config.coder_auto'], ...]. */
export const keyedOptions = (t, pairs) => pairs.map(([value, key]) => ({ value, label: t(key) }))
