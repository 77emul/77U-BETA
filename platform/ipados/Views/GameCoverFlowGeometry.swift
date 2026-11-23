// Cover-flow interaction adapted from the MeloNX library.
import Foundation

enum GameCoverFlowGeometry {
    static func initialSelectionID(savedID: String, gameIDs: [String]) -> String {
        gameIDs.contains(savedID) ? savedID : (gameIDs.first ?? "")
    }

    static func touchTarget(currentIndex: Int, gameCount: Int, translation: CGFloat,
                            velocity: CGFloat, step: CGFloat) -> Int {
        guard gameCount > 0, step > 0, translation.isFinite, velocity.isFinite else { return max(0, currentIndex) }
        let threshold = min(52, step * 0.18)
        let direction: Int
        if abs(translation) >= threshold {
            direction = translation > 0 ? -1 : 1
        } else if abs(translation) >= 12 && abs(velocity) >= 450 {
            direction = velocity > 0 ? -1 : 1
        } else { direction = 0 }
        return min(gameCount - 1, max(0, currentIndex + direction))
    }

    private static let settleThreshold: CGFloat = 0.28
    private static let velocityProjection: CGFloat = 0.28

    static func settledIndex(
        currentIndex: Int,
        gameCount: Int,
        translation: CGFloat,
        predictedTranslation: CGFloat,
        step: CGFloat
    ) -> Int {
        guard gameCount > 0, step.isFinite, step > 0 else { return 0 }

        let current = clampedIndex(currentIndex, gameCount: gameCount)
        let actual = translation.isFinite ? translation : 0
        let predicted = predictedTranslation.isFinite ? predictedTranslation : actual
        let projected = actual + (predicted - actual) * velocityProjection
        let projectedPages = projected / step

        guard abs(projectedPages) >= settleThreshold else { return current }

        let pageMagnitude = min(CGFloat(gameCount - 1), abs(projectedPages).rounded())
        let magnitude = max(1, Int(pageMagnitude))
        let movement = projectedPages < 0 ? magnitude : -magnitude
        return clampedIndex(current + movement, gameCount: gameCount)
    }

    static func rubberBandedTranslation(
        _ translation: CGFloat,
        currentIndex: Int,
        gameCount: Int,
        step: CGFloat
    ) -> CGFloat {
        guard gameCount > 0, step.isFinite, step > 0, translation.isFinite else { return 0 }

        let current = clampedIndex(currentIndex, gameCount: gameCount)
        let lowerBound = -CGFloat(gameCount - 1 - current) * step
        let upperBound = CGFloat(current) * step

        if translation < lowerBound {
            return lowerBound - softenedExcess(lowerBound - translation, step: step)
        }
        if translation > upperBound {
            return upperBound + softenedExcess(translation - upperBound, step: step)
        }
        return translation
    }

    static func liveCenterIndex(
        currentIndex: Int,
        gameCount: Int,
        translation: CGFloat,
        step: CGFloat
    ) -> CGFloat {
        guard gameCount > 0, step.isFinite, step > 0 else { return 0 }

        let current = clampedIndex(currentIndex, gameCount: gameCount)
        let safeTranslation = translation.isFinite ? translation : 0
        return min(
            CGFloat(gameCount - 1),
            max(0, CGFloat(current) - safeTranslation / step)
        )
    }

    static func visibleIndices(
        currentIndex: Int,
        gameCount: Int,
        translation: CGFloat,
        step: CGFloat,
        radius: Int = 4
    ) -> Range<Int> {
        guard gameCount > 0 else { return 0..<0 }

        let safeRadius = max(0, radius)
        let center = Int(
            liveCenterIndex(
                currentIndex: currentIndex,
                gameCount: gameCount,
                translation: translation,
                step: step
            ).rounded()
        )
        let lowerBound = max(0, center - safeRadius)
        let upperBound = min(gameCount, center + safeRadius + 1)
        return lowerBound..<upperBound
    }

    private static func softenedExcess(_ excess: CGFloat, step: CGFloat) -> CGFloat {
        (excess * 0.32) / (1 + excess / (step * 1.1))
    }

    private static func clampedIndex(_ index: Int, gameCount: Int) -> Int {
        min(gameCount - 1, max(0, index))
    }
}
