const MAX_SAMPLE_PIXELS = 5000
const MAX_ITERATIONS = 10

export const rgbToHsl = (r, g, b) => {
  r /= 255
  g /= 255
  b /= 255

  const max = Math.max(r, g, b)
  const min = Math.min(r, g, b)
  const l = (max + min) / 2
  const d = max - min

  if (d === 0) return [0, 0, l * 100]

  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
  let h

  switch (max) {
    case r:
      h = ((g - b) / d + (g < b ? 6 : 0)) / 6
      break
    case g:
      h = ((b - r) / d + 2) / 6
      break
    default:
      h = ((r - g) / d + 4) / 6
      break
  }

  return [h * 360, s * 100, l * 100]
}

export const colorDistance = (a, b) =>
  Math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)

const samplePixels = (imageData, maxPixels) => {
  const pixels = []
  const data = imageData?.data || []
  const pixelCount = Math.floor(data.length / 4)
  const step = Math.max(1, Math.floor(pixelCount / maxPixels))

  for (let pixelIndex = 0; pixelIndex < pixelCount; pixelIndex += step) {
    const index = pixelIndex * 4
    const alpha = data[index + 3]
    if (alpha < 128) continue
    pixels.push([data[index], data[index + 1], data[index + 2]])
  }

  return pixels
}

// Match the desktop UI's K-means++ palette extraction.
export const extractColors = (imageData, k = 5, random = Math.random) => {
  const pixels = samplePixels(imageData, MAX_SAMPLE_PIXELS)
  if (pixels.length < k) return pixels

  const centers = [pixels[Math.floor(random() * pixels.length)]]
  for (let centerIndex = 1; centerIndex < k; centerIndex++) {
    const distances = pixels.map((pixel) => Math.min(...centers.map((center) => colorDistance(pixel, center))))
    const totalDistance = distances.reduce((sum, distance) => sum + distance, 0)
    let target = random() * totalDistance

    for (let pixelIndex = 0; pixelIndex < distances.length; pixelIndex++) {
      target -= distances[pixelIndex]
      if (target <= 0) {
        centers.push(pixels[pixelIndex])
        break
      }
    }

    if (centers.length === centerIndex) centers.push(pixels[centerIndex % pixels.length])
  }

  for (let iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
    const clusters = Array.from({ length: k }, () => [])

    for (const pixel of pixels) {
      let minDistance = Infinity
      let nearestCenter = 0

      for (let centerIndex = 0; centerIndex < k; centerIndex++) {
        const distance = colorDistance(pixel, centers[centerIndex])
        if (distance < minDistance) {
          minDistance = distance
          nearestCenter = centerIndex
        }
      }

      clusters[nearestCenter].push(pixel)
    }

    let changed = false
    for (let centerIndex = 0; centerIndex < k; centerIndex++) {
      const cluster = clusters[centerIndex]
      if (!cluster.length) continue

      const average = [0, 1, 2].map(
        (channel) => cluster.reduce((sum, pixel) => sum + pixel[channel], 0) / cluster.length,
      )
      if (colorDistance(average, centers[centerIndex]) > 1) changed = true
      centers[centerIndex] = average
    }

    if (!changed) break
  }

  return centers.sort((a, b) => {
    const [, saturationA, lightnessA] = rgbToHsl(...a)
    const [, saturationB, lightnessB] = rgbToHsl(...b)
    return saturationB * lightnessB - saturationA * lightnessA
  })
}

export const selectAccentColor = (colors = []) => {
  const ranked = colors
    .map((color) => {
      const [, saturation, lightness] = rgbToHsl(...color)
      return {
        color,
        saturation,
        lightness,
        score: saturation * (lightness > 15 && lightness < 85 ? 1 : 0.3),
      }
    })
    .sort((a, b) => b.score - a.score)

  return ranked[0]?.color || colors[0] || null
}
