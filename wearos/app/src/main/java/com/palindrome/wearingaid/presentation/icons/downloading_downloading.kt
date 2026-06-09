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
public val downloading: ImageVector
  get() {
    if (_downloading != null) {
      return _downloading!!
    }
    _downloading =
      ImageVector.Builder(
          name = "downloading",
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
            moveTo(10.98f, 21.95f)
            quadTo(9.08f, 21.75f, 7.44f, 20.89f)
            reflectiveQuadTo(4.6f, 18.69f)
            reflectiveQuadTo(2.71f, 15.63f)
            reflectiveQuadTo(2.03f, 11.98f)
            quadTo(2.03f, 8.1f, 4.59f, 5.26f)
            reflectiveQuadTo(11f, 2f)
            verticalLineTo(4f)
            quadTo(7.98f, 4.42f, 6f, 6.69f)
            quadTo(4.03f, 8.95f, 4.03f, 11.98f)
            reflectiveQuadTo(6f, 17.26f)
            reflectiveQuadToRelative(4.98f, 2.69f)
            verticalLineToRelative(2f)
            close()
            moveToRelative(1f, -4.95f)
            lineTo(6.95f, 11.95f)
            lineTo(8.38f, 10.52f)
            lineToRelative(2.6f, 2.6f)
            verticalLineTo(7f)
            horizontalLineToRelative(2f)
            verticalLineToRelative(6.13f)
            lineToRelative(2.57f, -2.58f)
            lineTo(16.98f, 12f)
            lineToRelative(-5f, 5f)
            close()
            moveToRelative(1f, 4.95f)
            verticalLineToRelative(-2f)
            quadToRelative(1.07f, -0.15f, 2.06f, -0.57f)
            reflectiveQuadTo(16.88f, 18.3f)
            lineToRelative(1.45f, 1.45f)
            quadToRelative(-1.17f, 0.93f, -2.52f, 1.49f)
            reflectiveQuadToRelative(-2.82f, 0.71f)
            close()
            moveTo(16.93f, 5.65f)
            quadTo(16.05f, 5f, 15.06f, 4.57f)
            reflectiveQuadTo(13f, 4f)
            verticalLineTo(2f)
            quadToRelative(1.48f, 0.15f, 2.83f, 0.71f)
            reflectiveQuadToRelative(2.5f, 1.49f)
            lineToRelative(-1.4f, 1.45f)
            close()
            moveToRelative(2.8f, 12.65f)
            lineToRelative(-1.4f, -1.43f)
            quadToRelative(0.65f, -0.85f, 1.05f, -1.84f)
            quadToRelative(0.4f, -0.99f, 0.55f, -2.06f)
            horizontalLineToRelative(2.05f)
            quadToRelative(-0.2f, 1.47f, -0.75f, 2.84f)
            reflectiveQuadToRelative(-1.5f, 2.49f)
            close()
            moveToRelative(0.2f, -7.33f)
            quadTo(19.78f, 9.9f, 19.38f, 8.91f)
            quadTo(18.98f, 7.93f, 18.33f, 7.07f)
            lineToRelative(1.4f, -1.43f)
            quadToRelative(0.95f, 1.13f, 1.52f, 2.49f)
            reflectiveQuadToRelative(0.73f, 2.84f)
            horizontalLineTo(19.93f)
            close()
          }
        }
        .build()
    return _downloading!!
  }

private var _downloading: ImageVector? = null
