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
public val arrow_left: ImageVector
  get() {
    if (_arrow_left != null) {
      return _arrow_left!!
    }
    _arrow_left =
      ImageVector.Builder(
          name = "arrow_left",
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
            moveTo(14f, 17f)
            lineTo(9f, 12f)
            lineTo(14f, 7f)
            verticalLineTo(17f)
            close()
          }
        }
        .build()
    return _arrow_left!!
  }

private var _arrow_left: ImageVector? = null
