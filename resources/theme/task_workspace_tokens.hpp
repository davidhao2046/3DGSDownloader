#pragma once
#include "task_list_tokens.hpp"
namespace gsdl::theme::taskWorkspace {
// Reserved operation columns never change the parent height or title width.
inline constexpr float controlHeight=32,arrowInset=8,arrowWidth=32;
inline constexpr float contentInset=48,titleTop=8,titleHeight=23,metaTop=32,metaHeight=20;
inline constexpr float actionInset=12,actionWidth=108,actionGap=8,statusWidth=92,statusGap=12;
inline constexpr float iconSize=16,fileInset=16,fileNameInset=44;
inline constexpr float fileStatusWidth=188,fileSizeWidth=88,fileColumnGap=16,fileEndInset=16;
inline constexpr float sectionGap=8,sectionHeight=22,emptyWidth=440,emptyHeight=24;
inline constexpr float headerTop=22,headerHeight=32,countGap=12,countWidth=32,progressHeight=2,progressBottom=6;
inline constexpr float footerActionHeight=32;
}
