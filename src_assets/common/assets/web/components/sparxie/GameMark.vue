<template>
  <span class="gs-mark" :class="{ 'is-held': held }">
    <span class="gs-mark__text">{{ held ? words.changed : words.follow }}</span>
    <button v-if="held && !disabled" type="button" class="gs-mark__back" :aria-label="`${name}: ${words.back}`" :title="words.back" @click.stop="emit('reset')"><MIcon name="undo" /></button>
  </span>
</template>

<script setup>
import { MIcon } from 'foundation-ui/vue'

// The mark of one item of the game settings: 跟随游戏 while the application holds no value of its own (the item shows what the game has now), 已更改 once it does, with a way back.
defineProps({
  held: { type: Boolean, default: false },
  words: { type: Object, required: true },
  name: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
})
const emit = defineEmits(['reset'])
</script>
