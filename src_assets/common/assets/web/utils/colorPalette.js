const MAX_SAMPLE_PIXELS = 5000
const MAX_ITERATIONS = 10
const MIN_ALPHA = 128
const CONVERGENCE_DISTANCE = 1
const RGB_CHANNELS = [0, 1, 2]

export const rgbToHsl = (r, g, b) => {
  r /= 255
  g /= 255
  b /= 255

  const max = Math.max(r, g, b)
  const min = Math.min(r, g, b)
  const lightness = (max + min) / 2
  const delta = max - min

  if (delta === 0) return [0, 0, lightness * 100]

  const saturation = lightness > 0.5 ? delta / (2 - max - min) : delta / (max + min)
  let hue

  switch (max) {
    case r:
      hue = ((g - b) / delta + (g < b ? 6 : 0)) / 6
      break
    case g:
      hue = ((b - r) / delta + 2) / 6
      break
    default:
      hue = ((r - g) / delta + 4) / 6
      break
  }

  return [hue * 360, saturation * 100, lightness * 100]
}

export const colorDistance = (a, b) =>
  Math.sqrt(RGB_CHANNELS.reduce((sum, channel) => sum + (a[channel] - b[channel]) ** 2, 0))

const samplePixels = (imageData, maxPixels = MAX_SAMPLE_PIXELS) => {
  const data = imageData?.data
  if (!data?.length || maxPixels <= 0) return []

  const pixels = []
  const pixelCount = Math.floor(data.length / 4)
  const step = Math.max(1, Math.floor(pixelCount / maxPixels))

  for (let pixelIndex = 0; pixelIndex < pixelCount; pixelIndex += step) {
    const index = pixelIndex * 4
    if (data[index + 3] < MIN_ALPHA) continue
    pixels.push([data[index], data[index + 1], data[index + 2]])
  }

  return pixels
}

const randomIndex = (random, length) => Math.min(length - 1, Math.floor(Math.max(0, random()) * length))

const nearestDistance = (pixel, centers) => {
  let minDistance = Infinity

  for (const center of centers) {
    minDistance = Math.min(minDistance, colorDistance(pixel, center))
  }

  return minDistance
}

const selectWeightedPixel = (pixels, distances, random) => {
  const totalDistance = distances.reduce((sum, distance) => sum + distance, 0)
  if (totalDistance === 0) return pixels[randomIndex(random, pixels.length)]

  let target = random() * totalDistance
  for (let pixelIndex = 0; pixelIndex < distances.length; pixelIndex++) {
    target -= distances[pixelIndex]
    if (target <= 0) return pixels[pixelIndex]
  }

  return pixels[pixels.length - 1]
}

const initializeCenters = (pixels, k, random) => {
  const centers = [pixels[randomIndex(random, pixels.length)]]

  while (centers.length < k) {
    const distances = pixels.map((pixel) => nearestDistance(pixel, centers))
    centers.push(selectWeightedPixel(pixels, distances, random))
  }

  return centers
}

const assignClusters = (pixels, centers) => {
  const clusters = Array.from({ length: centers.length }, () => [])

  for (const pixel of pixels) {
    let minDistance = Infinity
    let nearestCenter = 0

    centers.forEach((center, centerIndex) => {
      const distance = colorDistance(pixel, center)
      if (distance < minDistance) {
        minDistance = distance
        nearestCenter = centerIndex
      }
    })

    clusters[nearestCenter].push(pixel)
  }

  return clusters
}

const updateCenters = (clusters, centers) => {
  let changed = false

  clusters.forEach((cluster, centerIndex) => {
    if (!cluster.length) return

    const average = RGB_CHANNELS.map(
      (channel) => cluster.reduce((sum, pixel) => sum + pixel[channel], 0) / cluster.length,
    )
    if (colorDistance(average, centers[centerIndex]) > CONVERGENCE_DISTANCE) changed = true
    centers[centerIndex] = average
  })

  return changed
}

const vividnessScore = (color) => {
  const [, saturation, lightness] = rgbToHsl(...color)
  return saturation * lightness
}

// Match the desktop UI's K-means++ palette extraction.
export const extractColors = (imageData, k = 5, random = Math.random) => {
  if (!Number.isInteger(k) || k <= 0) return []

  const pixels = samplePixels(imageData)
  if (pixels.length < k) return pixels

  const centers = initializeCenters(pixels, k, random)
  for (let iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
    const clusters = assignClusters(pixels, centers)
    if (!updateCenters(clusters, centers)) break
  }

  return centers.sort((a, b) => vividnessScore(b) - vividnessScore(a))
}

const accentScore = (color) => {
  const [, saturation, lightness] = rgbToHsl(...color)
  return saturation * (lightness > 15 && lightness < 85 ? 1 : 0.3)
}

export const selectAccentColor = (colors = []) => {
  const validColors = colors.filter((color) => Array.isArray(color) && color.length >= 3)
  return [...validColors].sort((a, b) => accentScore(b) - accentScore(a))[0] || null
}
