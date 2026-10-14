#include <memory>

#include "WinDic.h"

namespace {
	std::unique_ptr<WinDic> winDic;
}
WinDic::WinDic()
{
}
WinDic::~WinDic()
{
}

void WinDic::init()
{
	if (!winDic)
	{
		WinDic* rawPtr = new WinDic();
		winDic = std::unique_ptr<WinDic>(rawPtr);
	}
}

WinDic* WinDic::get()
{
	return winDic.get();
}

