// The backend of the developer-mode GitHub flows (sign-in, the Issue and PR reads through the Worker, the feedback sent under the person's name): the Foundation Sunshine Dev Panel GitHub App of AlkaidLab with the Device Flow,
// and the Worker in front of it. foundation-ui keeps these empty (it belongs to no product); each product gives its own once, before the first window is drawn.
export const DEV_BACKEND = Object.freeze({ workerUrl: 'https://devapi.alkaidlab.com', clientId: 'Iv23lioKKQ0XBqmts7yF' })
/** `configure` is foundation-ui's configureDevBackend. */
export const applyDevBackend = (configure) => configure({ ...DEV_BACKEND })
