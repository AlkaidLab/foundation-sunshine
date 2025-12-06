<template>
  <div class="accordion-item">
    <h2 class="accordion-header" :id="id + 'Heading'">
      <button
        class="accordion-button"
        :class="{ collapsed: !show }"
        type="button"
        data-bs-toggle="collapse"
        :data-bs-target="'#' + id + 'Collapse'"
        :aria-expanded="show"
        :aria-controls="id + 'Collapse'"
      >
        <i :class="['fas', icon, 'me-2']"></i>{{ title }}
      </button>
    </h2>
    <div
      :id="id + 'Collapse'"
      class="accordion-collapse collapse"
      :class="{ show }"
      :aria-labelledby="id + 'Heading'"
      :data-bs-parent="'#' + parentId"
    >
      <div class="accordion-body">
        <slot></slot>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'AccordionItem',
  props: {
    id: { type: String, required: true },
    icon: { type: String, required: true },
    title: { type: String, required: true },
    parentId: { type: String, required: true },
    show: { type: Boolean, default: false },
  },
}
</script>
