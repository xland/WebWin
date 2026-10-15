#pragma once
#include <array>
#include <string>

class Config
{
public:
	~Config();
	static void init();
	static Config* get();
	const std::wstring& getId() const;
	const std::wstring& getName() const;
	const std::array<int, 3>& getVersion() const;
	bool getQuitWhenNoWindow() const;
private:
	Config();
	void readPackageJson();
private:
	std::wstring id;
	std::wstring name;
	std::array<int, 3> version{ 1, 1, 1 };
	bool quitWhenNoWindow{ true };
};
