export const validateMatLiveSettings = (config) => {
  const pipe = config.mat_live_pipe ?? ''
  const producer = config.mat_live_producer ?? ''
  return Boolean(pipe) === Boolean(producer) ? '' : 'config.mat_live_pair_required'
}
