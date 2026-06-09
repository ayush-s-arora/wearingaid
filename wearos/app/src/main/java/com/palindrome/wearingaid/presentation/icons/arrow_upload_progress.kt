package com.palindrome.wearingaid.presentation.icons

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

@Suppress("CheckReturnValue")
public val arrow_upload_progress: ImageVector
  get() {
    if (_arrow_upload_progress != null) {
      return _arrow_upload_progress!!
    }
    _arrow_upload_progress =
      ImageVector.Builder(
          name = "arrow_upload_progress",
          defaultWidth = 24.dp,
          defaultHeight = 24.dp,
          viewportWidth = 24f,
          viewportHeight = 24f,
        )
        .apply {
          path(
            fill = SolidColor(Color.Black),
            fillAlpha = 1f,
            stroke = null,
            strokeAlpha = 1f,
            strokeLineWidth = 1f,
            strokeLineCap = StrokeCap.Butt,
            strokeLineJoin = StrokeJoin.Bevel,
            strokeLineMiter = 1f,
            pathFillType = PathFillType.Companion.NonZero,
          ) {
            moveTo(11.03f, 21.95f)
            quadTo(9.13f, 21.75f, 7.49f, 20.91f)
            reflectiveQuadTo(4.63f, 18.74f)
            reflectiveQuadTo(2.7f, 15.68f)
            reflectiveQuadTo(2f, 12f)
            quadTo(2f, 8.07f, 4.61f, 5.25f)
            quadTo(7.23f, 2.42f, 11.03f, 2.05f)
            verticalLineTo(4.07f)
            quadToRelative(-2.97f, 0.38f, -5f, 2.61f)
            quadTo(4f, 8.92f, 4f, 12f)
            reflectiveQuadToRelative(2.03f, 5.31f)
            reflectiveQuadToRelative(5f, 2.61f)
            verticalLineToRelative(2.02f)
            close()
            moveToRelative(0f, -4.95f)
            verticalLineTo(10.83f)
            lineToRelative(-2.6f, 2.6f)
            lineTo(7.03f, 12f)
            lineToRelative(5f, -5f)
            lineToRelative(5f, 5f)
            lineTo(15.6f, 13.4f)
            lineTo(13.03f, 10.83f)
            verticalLineTo(17f)
            horizontalLineToRelative(-2f)
            close()
            moveToRelative(2f, 4.95f)
            verticalLineTo(19.93f)
            quadToRelative(1.1f, -0.13f, 2.09f, -0.55f)
            reflectiveQuadTo(16.93f, 18.3f)
            lineToRelative(1.43f, 1.45f)
            quadToRelative(-1.13f, 0.9f, -2.48f, 1.48f)
            quadToRelative(-1.35f, 0.57f, -2.85f, 0.72f)
            close()
            moveTo(16.9f, 5.7f)
            quadTo(16.08f, 5.05f, 15.1f, 4.63f)
            reflectiveQuadTo(13.03f, 4.07f)
            verticalLineTo(2.05f)
            quadToRelative(1.5f, 0.15f, 2.85f, 0.73f)
            reflectiveQuadToRelative(2.48f, 1.48f)
            lineTo(16.9f, 5.7f)
            close()
            moveToRelative(2.85f, 12.63f)
            lineTo(18.33f, 16.9f)
            quadToRelative(0.65f, -0.82f, 1.08f, -1.81f)
            reflectiveQuadTo(19.95f, 13f)
            horizontalLineToRelative(2.02f)
            quadToRelative(-0.15f, 1.5f, -0.74f, 2.85f)
            reflectiveQuadToRelative(-1.49f, 2.47f)
            close()
            moveTo(19.95f, 11f)
            quadTo(19.83f, 9.9f, 19.4f, 8.91f)
            reflectiveQuadTo(18.33f, 7.1f)
            lineTo(19.75f, 5.68f)
            quadToRelative(0.9f, 1.13f, 1.49f, 2.47f)
            reflectiveQuadTo(21.98f, 11f)
            horizontalLineTo(19.95f)
            close()
          }
        }
        .build()
    return _arrow_upload_progress!!
  }

private var _arrow_upload_progress: ImageVector? = null
