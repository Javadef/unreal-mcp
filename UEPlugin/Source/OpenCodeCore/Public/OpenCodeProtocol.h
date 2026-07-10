#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

struct FOpenCodeRequest
{
	FString Id;
	FString Tool;
	TSharedPtr<FJsonObject> Args;

	bool ParseFromJson(const FString& JsonString)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}
		Root->TryGetStringField(TEXT("id"), Id);
		Root->TryGetStringField(TEXT("tool"), Tool);
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("args"), ArgsPtr))
		{
			Args = *ArgsPtr;
		}
		else
		{
			Args = MakeShareable(new FJsonObject());
		}
		return !Id.IsEmpty() && !Tool.IsEmpty();
	}
};

struct FOpenCodeResponse
{
	FString Id;
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Data;
	FString Error;

	FString ToJson() const
	{
		TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
		Root->SetStringField(TEXT("id"), Id);
		Root->SetBoolField(TEXT("success"), bSuccess);
		if (bSuccess && Data.IsValid())
		{
			Root->SetObjectField(TEXT("data"), Data);
		}
		else if (!bSuccess)
		{
			Root->SetStringField(TEXT("error"), Error);
		}
		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Root, Writer);
		return Output;
	}

	static FOpenCodeResponse Success(const FString& InId, TSharedPtr<FJsonObject> InData)
	{
		FOpenCodeResponse R;
		R.Id = InId;
		R.bSuccess = true;
		R.Data = InData;
		return R;
	}

	static FOpenCodeResponse Failure(const FString& InId, const FString& InError)
	{
		FOpenCodeResponse R;
		R.Id = InId;
		R.bSuccess = false;
		R.Error = InError;
		return R;
	}
};

static FString JSON_OBJ_TO_STRING(TSharedPtr<FJsonObject> Obj)
{
	if (!Obj.IsValid()) return TEXT("{}");
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}
