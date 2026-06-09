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
public val arrow_right: ImageVector
  get() {
    if (_arrow_right != null) {
      return _arrow_right!!
    }
    _arrow_right =
      ImageVector.Builder(
          name = "arrow_right",
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
            moveTo(10f, 17f)
            verticalLineTo(7f)
            lineToRelative(5f, 5f)
            lineToRelative(-5f, 5f)
            close()
          }
        }
        .build()
    return _arrow_right!!
  }

private var _arrow_right: ImageVector? = null
