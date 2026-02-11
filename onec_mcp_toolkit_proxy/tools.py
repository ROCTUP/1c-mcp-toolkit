"""
MCP Tool schemas and validation for 1C MCP Toolkit Proxy.

This module defines Pydantic models for MCP tool input validation.
These schemas ensure proper parameter validation before commands are sent to 1C.

Validates: Requirements 2.1, 2.5, 2.6, 3.1, 4.1
"""

import re
import json
from typing import Any, Dict, List, Optional, Union
from pydantic import BaseModel, Field, StrictBool, field_validator, model_validator


# Valid event log levels
VALID_EVENT_LOG_LEVELS = {"Information", "Warning", "Error", "Note"}

# Valid application names for event log filtering
VALID_APPLICATIONS = [
    "ThinClient", "WebClient", "ThickClient",
    "BackgroundJob", "Designer", "COMConnection",
    # Server-side and other clients
    "Server", "WebService", "HTTPService", "ODataInterface",
    # Mobile clients
    "MobileAppClient", "MobileAppServer", "MobileAppBackgroundJob",
    "MobileClient", "MobileStandaloneServer",
    # File variant clients
    "FileVariantBackgroundJob", "FileVariantServerSide",
    "WebSocket", "FileVariantWebSocket",
    # 1C Specific identifiers
    "1CV8C", "1CV8"
]

# Valid transaction statuses for event log filtering
VALID_TRANSACTION_STATUSES = [
    "Committed", "RolledBack", "NotApplicable", "Unfinished"
]

# ISO 8601 datetime pattern (YYYY-MM-DDTHH:MM:SS)
ISO_8601_PATTERN = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$"
)

# HexGUID pattern (32 hexadecimal characters)
HEXGUID_PATTERN = re.compile(r"^[0-9a-fA-F]{32}$")

# Valid search scopes for find_references_to_object
VALID_SEARCH_SCOPES = [
    "documents", "catalogs", "information_registers",
    "accumulation_registers", "accounting_registers", "calculation_registers"
]

# Valid metadata type prefixes for meta_filter.names validation
VALID_META_TYPE_PREFIXES = [
    "Документ", "Справочник", "РегистрСведений",
    "РегистрНакопления", "РегистрБухгалтерии", "РегистрРасчета",
    "ПланВидовХарактеристик", "ПланСчетов", "ПланВидовРасчета", "ПланОбмена",
    "БизнесПроцесс", "Задача",
    "Константа", "Перечисление",
    "Отчет", "Обработка",
    "РегламентноеЗадание", "ПараметрыСеанса"
]


# Valid detail sections for get_metadata(filter=...)
VALID_METADATA_SECTIONS = [
    "properties", "forms", "commands", "layouts", "predefined"
]
VALID_METADATA_SECTIONS_SET = set(VALID_METADATA_SECTIONS)



def validate_object_description_data(v: Dict[str, Any]) -> None:
    """
    Validate object_description structure (shared logic).
    
    Required fields:
    - _objectRef: must be True
    - УникальныйИдентификатор: UUID string (8-4-4-4-12)
    - ТипОбъекта: object type string
    """
    if not isinstance(v, dict):
        raise ValueError("object_description must be a dictionary")
    
    if "_objectRef" not in v:
        raise ValueError("object_description must contain '_objectRef' field")
    
    if v["_objectRef"] is not True:
        raise ValueError("object_description._objectRef must be true")
    
    if "УникальныйИдентификатор" not in v:
        raise ValueError("object_description must contain 'УникальныйИдентификатор' field")
    
    uuid_value = v["УникальныйИдентификатор"]
    if not isinstance(uuid_value, str) or not uuid_value.strip():
        raise ValueError("УникальныйИдентификатор must be a non-empty string")
    
    # Validate UUID format (8-4-4-4-12)
    uuid_pattern = re.compile(
        r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
    )
    if not uuid_pattern.match(uuid_value.strip()):
        raise ValueError(
            f"УникальныйИдентификатор must be a valid UUID (format: 8-4-4-4-12), got: '{uuid_value}'"
        )
    
    if "ТипОбъекта" not in v:
        raise ValueError("object_description must contain 'ТипОбъекта' field")
    
    obj_type = v["ТипОбъекта"]
    if not isinstance(obj_type, str) or not obj_type.strip():
        raise ValueError("ТипОбъекта must be a non-empty string")


class ExecuteQueryParams(BaseModel):
    """
    Parameters for the execute_query MCP tool.
    
    Validates: Requirements 2.1, 2.5, 2.6
    - 2.1: MCP-сервер предоставляет инструмент execute_query с параметром query
    - 2.5: Поддерживаются параметры запроса (передаются как дополнительный аргумент)
    - 2.6: Результат ограничивается разумным количеством строк с возможностью указать лимит
    """
    
    query: str = Field(
        ...,
        description="Текст запроса 1С (1C query language text)",
        min_length=1,
        examples=["ВЫБРАТЬ * ИЗ Справочник.Номенклатура"]
    )
    
    params: Optional[Dict[str, Any]] = Field(
        default=None,
        description="Параметры запроса (опционально) / Query parameters (optional)",
        examples=[{"КодТовара": "001", "Дата": "2024-01-01"}]
    )
    
    limit: int = Field(
        default=100,
        description="Максимальное количество строк / Maximum number of rows",
        ge=1,
        le=1000
    )
    
    include_schema: StrictBool = Field(
        default=False,
        description="Включить схему типов колонок в ответ / Include column type schema in response"
    )
    
    @field_validator('query')
    @classmethod
    def validate_query_not_empty(cls, v: str) -> str:
        """Validate that query is not empty or whitespace only."""
        if not v or not v.strip():
            raise ValueError("Query cannot be empty or whitespace only")
        return v.strip()
    
    @field_validator('params')
    @classmethod
    def validate_params(cls, v: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
        """Validate that params is a valid dictionary if provided."""
        if v is None:
            return None
        if not isinstance(v, dict):
            raise ValueError("Params must be a dictionary")
        return v


class ExecuteCodeParams(BaseModel):
    """
    Parameters for the execute_code MCP tool.
    
    Validates: Requirements 3.1
    - 3.1: MCP-сервер предоставляет инструмент execute_code с параметром code
    """
    
    code: str = Field(
        ...,
        description="Код 1С для выполнения / 1C code to execute",
        min_length=1,
        examples=["Результат = ТекущаяДата();"]
    )
    
    @field_validator('code')
    @classmethod
    def validate_code_not_empty(cls, v: str) -> str:
        """Validate that code is not empty or whitespace only."""
        if not v or not v.strip():
            raise ValueError("Code cannot be empty or whitespace only")
        return v.strip()


class GetMetadataParams(BaseModel):
    """
    Parameters for the get_metadata MCP tool.

    Validates: Requirements 4.1
    - 4.1: MCP-сервер предоставляет инструмент get_metadata с опциональным параметром filter
    """

    filter: Optional[str] = Field(
        default=None,
        description="Фильтр: имя объекта метаданных / Filter: metadata object name",
        examples=["Справочник.Номенклатура", "Catalog.Products"]
    )

    meta_type: Optional[Union[str, List[str]]] = Field(
        default=None,
        description="Тип объектов метаданных / Metadata object type",
        examples=["Справочник", ["Документ", "РегистрСведений", "РегистрНакопления"], "*"]
    )

    name_mask: Optional[str] = Field(
        default=None,
        description="Маска поиска по имени или синониму (регистронезависимый) / Name or synonym search mask (case-insensitive)",
        examples=["реализ", "номенклат", "контраг"]
    )

    limit: int = Field(
        default=100,
        description="Максимальное количество объектов в списке / Maximum number of objects in list",
        ge=1,
        le=1000
    )

    offset: int = Field(
        default=0,
        description="Смещение для постраничного вывода в режиме списка / Offset for pagination in list mode",
        ge=0,
        le=1000000
    )

    sections: Optional[List[str]] = Field(
        default=None,
        description=(
            "Секции детального ответа (работает только вместе с filter) / "
            "Detail sections (works only with filter)"
        ),
        examples=[["properties", "forms", "commands", "layouts", "predefined"]]
    )

    extension_name: Optional[str] = Field(
        default=None,
        description=(
            "Имя расширения конфигурации. "
            "None - основная конфигурация, "
            "'' (пустая строка) - список расширений, "
            "'ИмяРасширения' - объекты внутри расширения. "
            "Whitespace-only запрещён."
        )
    )

    @field_validator('filter')
    @classmethod
    def validate_filter(cls, v: Optional[str]) -> Optional[str]:
        """Validate and normalize filter parameter."""
        if v is None or v == "":
            return None
        return v.strip()

    @field_validator('meta_type')
    @classmethod
    def validate_meta_type(cls, v: Optional[Union[str, List[str]]]) -> Optional[Union[str, List[str]]]:
        """Validate and normalize meta_type parameter (string or list of strings)."""
        if v is None or v == "":
            return None
        if isinstance(v, str):
            return v.strip()
        if isinstance(v, list):
            cleaned = []
            for item in v:
                if item is None:
                    continue
                item_str = str(item).strip()
                if item_str == "":
                    continue
                cleaned.append(item_str)
            return cleaned or None
        # Anything else (e.g. int) -> coerce to string
        return str(v).strip() or None

    @field_validator('name_mask')
    @classmethod
    def validate_name_mask(cls, v: Optional[str]) -> Optional[str]:
        """Validate and normalize name_mask parameter."""
        if v is None or v == "":
            return None
        return v.strip()

    @field_validator('extension_name')
    @classmethod
    def validate_extension_name(cls, v: Optional[str]) -> Optional[str]:
        """Validate extension_name parameter.

        - None: main configuration (key not sent to 1C)
        - "": list of extensions (key sent with empty value)
        - "\"\"" / "''": compatibility mode for agents that over-escape empty string
        - "Name": objects inside extension (stripped)
        - Whitespace-only: ValueError (cannot be confused with empty string)
        """
        if v is None:
            return None
        # Empty string is valid - means "list of extensions"
        if v == "":
            return ""
        stripped = v.strip()
        # Compatibility: some agents send quoted empty string literal, e.g. "\"\""
        if stripped in ('""', "''"):
            return ""
        # PROTECTION: whitespace-only -> error (don't convert to "")
        if stripped == "":
            raise ValueError(
                "extension_name cannot be whitespace-only; "
                "use empty string '' for extensions list"
            )
        return stripped

    @field_validator('sections')
    @classmethod
    def validate_sections(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate and normalize sections parameter."""
        if v is None:
            return None

        cleaned: List[str] = []
        invalid: List[str] = []
        for item in v:
            if item is None:
                continue
            section = str(item).strip().lower()
            if section == "":
                continue
            if section not in VALID_METADATA_SECTIONS_SET:
                invalid.append(str(item))
                continue
            cleaned.append(section)

        if invalid:
            allowed = ", ".join(VALID_METADATA_SECTIONS)
            raise ValueError(
                f"Invalid sections: {invalid}. Allowed values: {allowed}"
            )

        # Remove duplicates while preserving order.
        return list(dict.fromkeys(cleaned)) or None

    @model_validator(mode='after')
    def validate_sections_consistency(self) -> 'GetMetadataParams':
        """sections can be used only in detail mode (with non-empty filter)."""
        if self.sections is not None and not self.filter:
            raise ValueError("sections parameter requires filter parameter")
        return self


class GetEventLogParams(BaseModel):
    """
    Parameters for the get_event_log MCP tool.
    Extended with additional filters.
    """
    
    # Existing parameters (unchanged)
    start_date: Optional[str] = Field(
        default=None,
        description="Дата начала периода в формате ISO 8601 (YYYY-MM-DDTHH:MM:SS)",
        examples=["2024-01-01T00:00:00"]
    )
    
    end_date: Optional[str] = Field(
        default=None,
        description="Дата окончания периода в формате ISO 8601",
        examples=["2024-01-31T23:59:59"]
    )
    
    levels: Optional[List[str]] = Field(
        default=None,
        description="Уровни важности: Information, Warning, Error, Note",
        examples=[["Error", "Warning"]]
    )
    
    events: Optional[List[str]] = Field(
        default=None,
        description="Фильтр по типу события",
        examples=[["_$Data$_.New", "_$Data$_.Update"]]
    )
    
    limit: int = Field(
        default=100,
        description="Максимальное количество записей",
        ge=1,
        le=1000
    )
    
    # Object filter parameters (priority: object_description > link > data)
    object_description: Optional[Dict[str, Any]] = Field(
        default=None,
        description="Описание объекта из результатов execute_query"
    )
    
    link: Optional[str] = Field(
        default=None,
        description="Навигационная ссылка в формате e1cib/data/..."
    )
    
    # Existing data parameter (kept for backward compatibility)
    data: Optional[str] = Field(
        default=None,
        description="Ссылка на объект данных (навигационная ссылка)"
    )
    
    metadata_type: Optional[List[str]] = Field(
        default=None,
        description="Тип объекта метаданных (например, Документ.РеализацияТоваровУслуг)"
    )

    @field_validator("metadata_type", mode="before")
    @classmethod
    def normalize_metadata_type(cls, v: Any) -> Any:
        """
        Normalize metadata_type input.

        Accepts:
        - list[str] (preferred)
        - single string (wrapped into a list)
        - stringified JSON array (e.g. "[\"Документ.ВходящийДокумент\"]")
          which can happen when clients double-serialize JSON.
        """
        if v is None or v == "":
            return None

        if isinstance(v, str):
            raw = v.strip()
            if raw.startswith("[") and raw.endswith("]"):
                try:
                    parsed = json.loads(raw)
                    if isinstance(parsed, list):
                        return parsed
                except Exception:
                    pass
            return [raw]

        return v
    
    user: Optional[List[str]] = Field(
        default=None,
        description="Имя пользователя (или массив имён)"
    )
    
    session: Optional[List[int]] = Field(
        default=None,
        description="Номер сеанса (или массив номеров)"
    )
    
    application: Optional[List[str]] = Field(
        default=None,
        description="Тип приложения: ThinClient, WebClient, ThickClient, BackgroundJob, Designer, COMConnection"
    )
    
    computer: Optional[str] = Field(
        default=None,
        description="Имя компьютера"
    )
    
    comment_contains: Optional[str] = Field(
        default=None,
        description="Подстрока для поиска в комментарии"
    )
    
    transaction_status: Optional[str] = Field(
        default=None,
        description="Статус транзакции: Committed, RolledBack, NotApplicable, Unfinished"
    )

    same_second_offset: int = Field(
        default=0,
        description="Пропустить N записей с той же секундой что и start_date (для пагинации)",
        ge=0,
        le=10000
    )

    @model_validator(mode='after')
    def validate_same_second_offset_requires_start_date(self) -> 'GetEventLogParams':
        """same_second_offset имеет смысл только при start_date."""
        if self.same_second_offset > 0 and not self.start_date:
            raise ValueError(
                "same_second_offset requires start_date to be specified"
            )
        return self

    @model_validator(mode='after')
    def validate_object_filter_params(self):
        """
        Validate only the object filter parameter that will be used (by priority).
        Priority: object_description > link > data
        """
        # Priority 1: object_description
        if self.object_description is not None:
            self._validate_object_description(self.object_description)
            return self
        
        # Priority 2: link
        if self.link is not None and self.link.strip():
            self._validate_link_format(self.link)
            return self
        
        # Priority 3: data (no additional validation, backward compatible)
        return self
    
    @staticmethod
    def _validate_object_description(v: Dict[str, Any]) -> None:
        """Validate object_description structure."""
        validate_object_description_data(v)
    
    @staticmethod
    def _validate_link_format(v: str) -> None:
        """Validate navigation link format."""
        v = v.strip()
        
        if not v.startswith("e1cib/data/"):
            raise ValueError("link must start with 'e1cib/data/'")
        
        if "?ref=" not in v:
            raise ValueError("link must contain '?ref=' parameter")
        
        ref_part = v.split("?ref=")[-1]
        if len(ref_part) != 32:
            raise ValueError(f"ref parameter must be exactly 32 hexadecimal characters, got {len(ref_part)}")
        
        if not HEXGUID_PATTERN.match(ref_part):
            raise ValueError("ref parameter must contain only hexadecimal characters")
    
    @field_validator('start_date', 'end_date')
    @classmethod
    def validate_iso8601_date(cls, v: Optional[str]) -> Optional[str]:
        """Validate that date is in ISO 8601 format (YYYY-MM-DDTHH:MM:SS)."""
        if v is None or v == "":
            return None
        
        v = v.strip()
        
        if not ISO_8601_PATTERN.match(v):
            raise ValueError(
                f"Invalid date format: '{v}'. Expected ISO 8601 format: YYYY-MM-DDTHH:MM:SS"
            )
        
        return v
    
    @field_validator('levels')
    @classmethod
    def validate_levels(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate that levels contain only valid values (Information, Warning, Error, Note)."""
        if v is None:
            return None
        
        if not isinstance(v, list):
            raise ValueError("Levels must be a list")
        
        if len(v) == 0:
            return None
        
        invalid_levels = []
        validated_levels = []
        
        for level in v:
            if not isinstance(level, str):
                raise ValueError(f"Level must be a string, got: {type(level).__name__}")
            
            level_stripped = level.strip()
            
            if level_stripped not in VALID_EVENT_LOG_LEVELS:
                invalid_levels.append(level_stripped)
            else:
                validated_levels.append(level_stripped)
        
        if invalid_levels:
            raise ValueError(
                f"Invalid level(s): {invalid_levels}. "
                f"Valid levels are: {sorted(VALID_EVENT_LOG_LEVELS)}"
            )
        
        return validated_levels if validated_levels else None
    
    @field_validator('events')
    @classmethod
    def validate_events(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate and normalize events parameter."""
        if v is None:
            return None
        
        if not isinstance(v, list):
            raise ValueError("Events must be a list")
        
        if len(v) == 0:
            return None
        
        validated_events = []
        for event in v:
            if not isinstance(event, str):
                raise ValueError(f"Event must be a string, got: {type(event).__name__}")
            event_stripped = event.strip()
            if event_stripped:
                validated_events.append(event_stripped)
        
        return validated_events if validated_events else None
    
    @field_validator('application')
    @classmethod
    def validate_application(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate that application values are from the allowed list."""
        if v is None:
            return None
        
        if not isinstance(v, list):
            raise ValueError("Application must be a list")
        
        if len(v) == 0:
            return None
        
        invalid_apps = []
        validated_apps = []
        
        for app in v:
            if not isinstance(app, str):
                raise ValueError(f"Application must be a string, got: {type(app).__name__}")
            
            app_stripped = app.strip()
            
            if app_stripped not in VALID_APPLICATIONS:
                invalid_apps.append(app_stripped)
            else:
                validated_apps.append(app_stripped)
        
        if invalid_apps:
            raise ValueError(
                f"Invalid application(s): {invalid_apps}. "
                f"Valid applications are: {VALID_APPLICATIONS}"
            )
        
        return validated_apps if validated_apps else None
    
    @field_validator('transaction_status')
    @classmethod
    def validate_transaction_status(cls, v: Optional[str]) -> Optional[str]:
        """Validate that transaction_status is from the allowed list."""
        if v is None or v == "":
            return None
        
        v = v.strip()
        
        if v not in VALID_TRANSACTION_STATUSES:
            raise ValueError(
                f"Invalid transaction_status: '{v}'. "
                f"Valid statuses are: {VALID_TRANSACTION_STATUSES}"
            )
        
        return v


class GetObjectByLinkParams(BaseModel):
    """
    Parameters for the get_object_by_link MCP tool.
    
    Validates: Requirements 1.2, 7.1, 7.2, 7.3, 7.4
    - 1.2: WHEN the `link` parameter is provided, THE MCP_Proxy SHALL validate it against the navigation link format
    - 7.1: WHEN the `link` parameter is empty or whitespace, THE MCP_Proxy SHALL return validation error
    - 7.2: WHEN the `link` parameter does not start with `e1cib/data/`, THE MCP_Proxy SHALL return format error
    - 7.3: WHEN the `ref` parameter is missing from the link, THE MCP_Proxy SHALL return error indicating missing reference
    - 7.4: WHEN the HexGUID is not exactly 32 hexadecimal characters, THE MCP_Proxy SHALL return error describing expected format
    """
    
    link: str = Field(
        ...,
        description="Навигационная ссылка 1С",
        min_length=1
    )
    
    @field_validator('link')
    @classmethod
    def validate_link_format(cls, v: str) -> str:
        """
        Validate navigation link format.
        
        Expected format: e1cib/data/ТипОбъекта.ИмяОбъекта?ref=HexGUID
        Example: e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5
        """
        # Strip whitespace
        v = v.strip()
        
        # Requirement 7.1: Check for empty or whitespace-only link
        if not v:
            raise ValueError("Link cannot be empty")
        
        # Requirement 7.2: Check for correct prefix
        if not v.startswith("e1cib/data/"):
            raise ValueError("Link must start with 'e1cib/data/'")
        
        # Requirement 7.3: Check for ref parameter
        if "?ref=" not in v:
            raise ValueError("Link must contain '?ref=' parameter")
        
        # Requirement 7.4: Extract and validate HexGUID
        ref_part = v.split("?ref=")[-1]
        
        # Validate HexGUID is exactly 32 hexadecimal characters
        if len(ref_part) != 32:
            raise ValueError(
                f"ref parameter must be exactly 32 hexadecimal characters, got {len(ref_part)}"
            )
        
        if not HEXGUID_PATTERN.match(ref_part):
            raise ValueError("ref parameter must contain only hexadecimal characters (0-9, a-f, A-F)")
        
        return v


class GetLinkOfObjectParams(BaseModel):
    """
    Parameters for the get_link_of_object MCP tool.
    
    Generates a navigation link from an object description returned by execute_query.
    This allows AI to provide users with clickable links to open objects in 1C.
    """
    
    object_description: Dict[str, Any] = Field(
        ...,
        description="Описание объекта из результатов execute_query с полями {_objectRef, УникальныйИдентификатор, ТипОбъекта}"
    )
    
    @field_validator('object_description')
    @classmethod
    def validate_object_description(cls, v: Dict[str, Any]) -> Dict[str, Any]:
        """
        Validate that object_description has required fields.
        
        Required fields:
        - _objectRef: must be True
        - УникальныйИдентификатор: UUID string
        - ТипОбъекта: object type string (e.g., "СправочникСсылка.Контрагенты")
        """
        validate_object_description_data(v)
        return v


class MetaFilter(BaseModel):
    """
    Sub-model for meta_filter parameter of find_references_to_object.

    Allows filtering metadata objects by exact names or by name mask.
    If names is provided, name_mask is ignored.
    """

    names: Optional[List[str]] = Field(
        default=None,
        description="Список точных имён объектов метаданных (например, ['Документ.РеализацияТоваровУслуг'])"
    )

    name_mask: Optional[str] = Field(
        default=None,
        description="Маска для поиска по имени или синониму (регистронезависимый)"
    )

    @field_validator('names')
    @classmethod
    def validate_names(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate meta_filter.names entries."""
        if v is None:
            return None

        if not isinstance(v, list):
            raise ValueError("names must be a list")

        if len(v) == 0:
            return None

        name_pattern = re.compile(r"^\w+\.\w+$")
        validated_names = []

        for name in v:
            if not isinstance(name, str):
                raise ValueError(f"Each name must be a string, got: {type(name).__name__}")

            name_stripped = name.strip()

            if not name_pattern.match(name_stripped):
                raise ValueError(
                    f"Invalid name format: '{name_stripped}'. "
                    f"Expected format: 'ТипМетаданных.ИмяОбъекта' (e.g., 'Документ.РеализацияТоваровУслуг')"
                )

            prefix = name_stripped.split(".")[0]
            if prefix not in VALID_META_TYPE_PREFIXES:
                raise ValueError(
                    f"Invalid metadata type prefix: '{prefix}'. "
                    f"Valid prefixes are: {VALID_META_TYPE_PREFIXES}"
                )

            validated_names.append(name_stripped)

        return validated_names if validated_names else None

    @field_validator('name_mask')
    @classmethod
    def validate_name_mask(cls, v: Optional[str]) -> Optional[str]:
        """Validate and normalize name_mask parameter."""
        if v is None or v == "":
            return None

        v = v.strip()

        if not v:
            raise ValueError("name_mask cannot be empty or whitespace only")

        return v


class GetAccessRightsParams(BaseModel):
    """
    Parameters for the get_access_rights MCP tool.

    Gets role permissions for a metadata object and optionally effective rights for a specific user.

    Limitations:
    - effective_rights is "rights by sum of roles", NOT a guarantee of real access
    - RLS (Row Level Security) is NOT taken into account
    - Contextual restrictions (by organizations, departments) are NOT taken into account
    - This is a check of "what is allowed by roles", not "what the user will actually see"

    Runtime requirements:
    - Admin rights are required: AccessRight() with 3rd parameter (Role/User) requires admin rights
    - Privileged mode is forbidden: if enabled, AccessRight() always returns True, result is meaningless
    """

    metadata_object: str = Field(
        ...,
        description="Полное имя объекта метаданных (например, Справочник.Контрагенты)",
        min_length=1,
        examples=["Справочник.Контрагенты", "Документ.РеализацияТоваровУслуг"]
    )

    user_name: Optional[str] = Field(
        default=None,
        description="Имя пользователя ИБ или наименование из справочника Пользователи (регистронезависимый поиск). Если указано, добавляются эффективные права пользователя.",
        examples=["Иванов", "Администратор", "Иванов Иван Иванович"]
    )

    rights_filter: Optional[List[str]] = Field(
        default=None,
        description="Показывать только эти права в результате. Пустой массив или null = фильтра нет (дефолтный список по типу).",
        examples=[["Чтение", "Изменение"], ["Чтение", "Добавление", "Удаление"]]
    )

    roles_filter: Optional[List[str]] = Field(
        default=None,
        description="Показывать только эти роли (точное совпадение, case-insensitive). Пустой массив или null = показывать все роли.",
        examples=[["ПолныеПрава", "Менеджер"], ["Администратор"]]
    )

    @field_validator('metadata_object')
    @classmethod
    def validate_metadata_object(cls, v: str) -> str:
        """Validate that metadata_object is not empty and contains a dot (Type.Name format)."""
        if not v or not v.strip():
            raise ValueError("metadata_object cannot be empty")
        v = v.strip()
        if '.' not in v:
            raise ValueError(
                f"metadata_object must be in format 'ТипМетаданных.ИмяОбъекта' (e.g., 'Справочник.Контрагенты'), got: '{v}'"
            )
        return v

    @field_validator('user_name')
    @classmethod
    def validate_user_name(cls, v: Optional[str]) -> Optional[str]:
        """Validate and normalize user_name parameter."""
        if v is None or v == "":
            return None
        stripped = v.strip()
        if not stripped:
            return None
        return stripped

    @field_validator('rights_filter')
    @classmethod
    def validate_rights_filter(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate and normalize rights_filter parameter."""
        if v is None:
            return None
        if not isinstance(v, list):
            raise ValueError("rights_filter must be a list")
        if len(v) == 0:
            return None

        validated_rights = []
        for right in v:
            if not isinstance(right, str):
                raise ValueError(f"Each right must be a string, got: {type(right).__name__}")
            right_stripped = right.strip()
            if right_stripped:
                validated_rights.append(right_stripped)

        return validated_rights if validated_rights else None

    @field_validator('roles_filter')
    @classmethod
    def validate_roles_filter(cls, v: Optional[List[str]]) -> Optional[List[str]]:
        """Validate and normalize roles_filter parameter."""
        if v is None:
            return None
        if not isinstance(v, list):
            raise ValueError("roles_filter must be a list")
        if len(v) == 0:
            return None

        validated_roles = []
        for role in v:
            if not isinstance(role, str):
                raise ValueError(f"Each role must be a string, got: {type(role).__name__}")
            role_stripped = role.strip()
            if role_stripped:
                validated_roles.append(role_stripped)

        return validated_roles if validated_roles else None


class FindReferencesToObjectParams(BaseModel):
    """
    Parameters for the find_references_to_object MCP tool.

    Finds references to a given object across specified metadata collections.
    """

    target_object_description: Dict[str, Any] = Field(
        ...,
        description="Описание целевого объекта из результатов execute_query с полями {_objectRef, УникальныйИдентификатор, ТипОбъекта}"
    )

    search_scope: List[str] = Field(
        ...,
        description="Области поиска: documents, catalogs, information_registers, accumulation_registers, accounting_registers, calculation_registers",
        min_length=1
    )

    meta_filter: Optional[MetaFilter] = Field(
        default=None,
        description="Фильтр объектов метаданных: {names?: string[], name_mask?: string}"
    )

    limit_hits: int = Field(
        default=200,
        description="Максимальное количество находок",
        ge=1,
        le=10000
    )

    limit_per_meta: int = Field(
        default=20,
        description="Максимальное количество находок на один объект метаданных",
        ge=1,
        le=1000
    )

    timeout_budget_sec: int = Field(
        default=30,
        description="Бюджет времени в секундах",
        ge=5,
        le=300
    )

    @field_validator('target_object_description')
    @classmethod
    def validate_target_object_description(cls, v: Dict[str, Any]) -> Dict[str, Any]:
        """Validate target_object_description structure."""
        cls._validate_object_description(v)
        return v

    @field_validator('search_scope')
    @classmethod
    def validate_search_scope(cls, v: List[str]) -> List[str]:
        """Validate that search_scope contains only valid values."""
        if not isinstance(v, list):
            raise ValueError("search_scope must be a list")

        if len(v) == 0:
            raise ValueError("search_scope must contain at least one element")

        invalid_scopes = []
        validated_scopes = []

        for scope in v:
            if not isinstance(scope, str):
                raise ValueError(f"Each scope must be a string, got: {type(scope).__name__}")

            scope_stripped = scope.strip()

            if scope_stripped not in VALID_SEARCH_SCOPES:
                invalid_scopes.append(scope_stripped)
            else:
                validated_scopes.append(scope_stripped)

        if invalid_scopes:
            raise ValueError(
                f"Invalid search scope(s): {invalid_scopes}. "
                f"Valid scopes are: {VALID_SEARCH_SCOPES}"
            )

        return validated_scopes

    @staticmethod
    def _validate_object_description(v: Dict[str, Any]) -> None:
        """Validate object_description structure."""
        validate_object_description_data(v)


# Tool schema definitions for MCP registration
# These match the JSON Schema format specified in the design document

EXECUTE_QUERY_SCHEMA = {
    "name": "execute_query",
    "description": "Выполняет запрос на языке запросов 1С и возвращает результат",
    "inputSchema": {
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "Текст запроса 1С"
            },
            "params": {
                "type": "object",
                "description": "Параметры запроса (опционально)"
            },
            "limit": {
                "type": "integer",
                "description": "Максимальное количество строк",
                "default": 100
            },
            "include_schema": {
                "type": "boolean",
                "description": "Включить схему типов колонок в ответ",
                "default": False
            }
        },
        "required": ["query"]
    }
}

EXECUTE_CODE_SCHEMA = {
    "name": "execute_code",
    "description": "Выполняет произвольный код на языке 1С (блок операторов через Выполнить). Ограничения: нельзя объявлять Процедура/Функция и нельзя использовать Возврат — для результата присваивайте значение переменной Результат.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "code": {
                "type": "string",
                "description": "Код 1С для выполнения. Используйте `Результат = ...;` чтобы вернуть значение. Не используйте `Возврат` и не объявляйте `Процедура/Функция`."
            }
        },
        "required": ["code"]
    }
}

GET_METADATA_SCHEMA = {
    "name": "get_metadata",
    "description": "Возвращает информацию о метаданных базы 1С. Без параметров (filter/meta_type/name_mask) возвращает сводку: data (Тип + Количество по корневым типам) + configuration (информация о конфигурации/версии платформы, если доступна) отдельным JSON-полем верхнего уровня. С параметром filter возвращает детальную структуру конкретного объекта (по полному имени) и поддерживает выбор секций через sections. Параметры meta_type и/или name_mask возвращают список объектов (для meta_type можно указать '*' чтобы перечислить объекты всех корневых типов). В режиме списка элементы содержат ПолноеИмя и Синоним; результаты стабильно сортируются по ПолноеИмя и поддерживают постраничный просмотр через offset. При обрезке по limit возвращаются поля truncated/limit/returned, а также offset/has_more/next_offset. Параметр extension_name позволяет работать с расширениями: не указан - основная конфигурация, '' - список расширений, 'Имя' - объекты расширения.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "filter": {
                "type": "string",
                "description": "Фильтр: точное имя объекта метаданных для получения детальной структуры (например, 'Справочник.Номенклатура')"
            },
            "meta_type": {
                "description": "Тип(ы) объектов метаданных для фильтрации списка. Можно строкой (например, 'Документ') или массивом строк (например, ['Документ','РегистрСведений']). Используйте '*' чтобы перечислить объекты по всем корневым типам.",
                "oneOf": [
                    {"type": "string"},
                    {"type": "array", "items": {"type": "string"}}
                ]
            },
            "name_mask": {
                "type": "string",
                "description": "Маска для поиска по имени или синониму объекта (регистронезависимый поиск подстроки)"
            },
            "limit": {
                "type": "integer",
                "description": "Максимальное количество объектов в списке",
                "default": 100
            },
            "offset": {
                "type": "integer",
                "description": "Смещение для постраничного вывода в режиме списка",
                "default": 0
            },
            "sections": {
                "type": "array",
                "items": {
                    "type": "string",
                    "enum": VALID_METADATA_SECTIONS
                },
                "description": "Секции детального ответа (работает только вместе с filter): properties, forms, commands, layouts, predefined"
            },
            "extension_name": {
                "type": "string",
                "description": (
                    "Имя расширения: не указано - основная конфигурация, "
                    "'' - список расширений, 'Имя' - объекты расширения. "
                    "Whitespace-only запрещён."
                )
            }
        }
    }
}

GET_EVENT_LOG_SCHEMA = {
    "name": "get_event_log",
    "description": "Получает записи из журнала регистрации 1С с расширенными возможностями фильтрации. Поддерживает курсорную пагинацию: ответ содержит last_date, next_same_second_offset и has_more. Для следующей страницы используйте last_date как start_date и next_same_second_offset как same_second_offset.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "start_date": {
                "type": "string",
                "description": "Дата начала периода (ISO 8601: YYYY-MM-DDTHH:MM:SS). При пагинации используйте last_date из предыдущего ответа"
            },
            "end_date": {
                "type": "string",
                "description": "Дата окончания периода (ISO 8601)"
            },
            "levels": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Уровни важности: Information, Warning, Error, Note"
            },
            "events": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Фильтр по типу события"
            },
            "limit": {
                "type": "integer",
                "description": "Максимальное количество записей",
                "default": 100
            },
            "same_second_offset": {
                "type": "integer",
                "description": "Пропустить N записей с той же секундой что и start_date (для пагинации). При пагинации используйте next_same_second_offset из предыдущего ответа",
                "default": 0
            },
            "object_description": {
                "type": "object",
                "description": "Описание объекта из результатов execute_query с полями {_objectRef, УникальныйИдентификатор, ТипОбъекта}",
                "properties": {
                    "_objectRef": {"type": "boolean"},
                    "УникальныйИдентификатор": {"type": "string"},
                    "ТипОбъекта": {"type": "string"},
                    "Представление": {"type": "string"}
                }
            },
            "link": {
                "type": "string",
                "description": "Навигационная ссылка в формате e1cib/data/ТипОбъекта.ИмяОбъекта?ref=HexGUID"
            },
            "data": {
                "type": "string",
                "description": "Ссылка на объект данных (навигационная ссылка) - для обратной совместимости"
            },
            "metadata_type": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Тип объекта метаданных (например, Документ.РеализацияТоваровУслуг)"
            },
            "user": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Имя пользователя"
            },
            "session": {
                "type": "array",
                "items": {"type": "integer"},
                "description": "Номер сеанса"
            },
            "application": {
                "type": "array",
                "items": {
                    "type": "string",
                    "enum": ["ThinClient", "WebClient", "ThickClient", "BackgroundJob", "Designer", "COMConnection"]
                },
                "description": "Тип приложения"
            },
            "computer": {
                "type": "string",
                "description": "Имя компьютера"
            },
            "comment_contains": {
                "type": "string",
                "description": "Подстрока для поиска в комментарии"
            },
            "transaction_status": {
                "type": "string",
                "enum": ["Committed", "RolledBack", "NotApplicable", "Unfinished"],
                "description": "Статус транзакции"
            }
        }
    }
}

GET_OBJECT_BY_LINK_SCHEMA = {
    "name": "get_object_by_link",
    "description": "Получает данные объекта 1С по навигационной ссылке",
    "inputSchema": {
        "type": "object",
        "properties": {
            "link": {
                "type": "string",
                "description": "Навигационная ссылка в формате e1cib/data/ТипОбъекта.ИмяОбъекта?ref=HexGUID"
            }
        },
        "required": ["link"]
    }
}

GET_LINK_OF_OBJECT_SCHEMA = {
    "name": "get_link_of_object",
    "description": "Генерирует навигационную ссылку 1С по описанию объекта. Используй чтобы дать пользователю кликабельную ссылку для открытия объекта в 1С. Принимает описание объекта из результатов execute_query.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "object_description": {
                "type": "object",
                "description": "Описание объекта с полями {_objectRef: true, УникальныйИдентификатор, ТипОбъекта, Представление}",
                "properties": {
                    "_objectRef": {
                        "type": "boolean",
                        "description": "Признак описания объекта (должен быть true)"
                    },
                    "УникальныйИдентификатор": {
                        "type": "string",
                        "description": "UUID объекта"
                    },
                    "ТипОбъекта": {
                        "type": "string",
                        "description": "Тип объекта (например, 'СправочникСсылка.Контрагенты')"
                    },
                    "Представление": {
                        "type": "string",
                        "description": "Строковое представление объекта"
                    }
                },
                "required": ["_objectRef", "УникальныйИдентификатор", "ТипОбъекта"]
            }
        },
        "required": ["object_description"]
    }
}

GET_ACCESS_RIGHTS_SCHEMA = {
    "name": "get_access_rights",
    "description": "Получает права ролей на объект метаданных и опционально эффективные права конкретного пользователя ИБ. ВАЖНО: effective_rights — это 'права по сумме ролей', НЕ гарантия реального доступа. RLS и контекстные ограничения НЕ учитываются. Требует административных прав; не работает в привилегированном режиме.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "metadata_object": {
                "type": "string",
                "description": "Полное имя объекта метаданных в формате 'ТипМетаданных.ИмяОбъекта' (например, 'Справочник.Контрагенты')"
            },
            "user_name": {
                "type": "string",
                "description": "Имя пользователя ИБ или наименование из справочника Пользователи (регистронезависимый поиск). Если указано, добавляются эффективные права пользователя."
            },
            "rights_filter": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Показывать только эти права в результате. Пустой массив или отсутствие = дефолтный список по типу объекта."
            },
            "roles_filter": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Показывать только эти роли (точное совпадение, case-insensitive). Пустой массив или отсутствие = все роли с правами."
            }
        },
        "required": ["metadata_object"]
    }
}

FIND_REFERENCES_TO_OBJECT_SCHEMA = {
    "name": "find_references_to_object",
    "description": "Находит ссылки на указанный объект в базе данных 1С. Ищет во всех полях указанных коллекций метаданных (документы, справочники, регистры), где тип поля может содержать ссылку на целевой объект.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "target_object_description": {
                "type": "object",
                "description": "Описание целевого объекта с полями {_objectRef: true, УникальныйИдентификатор, ТипОбъекта, Представление}",
                "properties": {
                    "_objectRef": {
                        "type": "boolean",
                        "description": "Признак описания объекта (должен быть true)"
                    },
                    "УникальныйИдентификатор": {
                        "type": "string",
                        "description": "UUID объекта"
                    },
                    "ТипОбъекта": {
                        "type": "string",
                        "description": "Тип объекта (например, 'СправочникСсылка.Контрагенты')"
                    },
                    "Представление": {
                        "type": "string",
                        "description": "Строковое представление объекта"
                    }
                },
                "required": ["_objectRef", "УникальныйИдентификатор", "ТипОбъекта"]
            },
            "search_scope": {
                "type": "array",
                "items": {
                    "type": "string",
                    "enum": ["documents", "catalogs", "information_registers", "accumulation_registers", "accounting_registers", "calculation_registers"]
                },
                "description": "Области поиска: documents (документы), catalogs (справочники), information_registers (регистры сведений), accumulation_registers (регистры накопления), accounting_registers (регистры бухгалтерии), calculation_registers (регистры расчёта)"
            },
            "meta_filter": {
                "type": "object",
                "description": "Фильтр объектов метаданных. Если names задан — используются только указанные имена, name_mask игнорируется.",
                "properties": {
                    "names": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Список точных имён объектов метаданных (например, ['Документ.РеализацияТоваровУслуг'])"
                    },
                    "name_mask": {
                        "type": "string",
                        "description": "Маска для поиска по имени или синониму (регистронезависимый)"
                    }
                }
            },
            "limit_hits": {
                "type": "integer",
                "description": "Максимальное количество находок",
                "default": 200
            },
            "limit_per_meta": {
                "type": "integer",
                "description": "Максимальное количество находок на один объект метаданных",
                "default": 20
            },
            "timeout_budget_sec": {
                "type": "integer",
                "description": "Бюджет времени в секундах (5–300)",
                "default": 30
            }
        },
        "required": ["target_object_description", "search_scope"]
    }
}

# List of all tool schemas
ALL_TOOL_SCHEMAS = [
    EXECUTE_QUERY_SCHEMA,
    EXECUTE_CODE_SCHEMA,
    GET_METADATA_SCHEMA,
    GET_EVENT_LOG_SCHEMA,
    GET_OBJECT_BY_LINK_SCHEMA,
    GET_LINK_OF_OBJECT_SCHEMA,
    FIND_REFERENCES_TO_OBJECT_SCHEMA,
    GET_ACCESS_RIGHTS_SCHEMA
]


def validate_execute_query_params(
    query: str,
    params: Optional[Dict[str, Any]] = None,
    limit: int = 1000,
    include_schema: bool = False
) -> ExecuteQueryParams:
    """
    Validate execute_query parameters using Pydantic model.
    
    Args:
        query: The query text
        params: Optional query parameters
        limit: Maximum number of rows
        include_schema: Include column type schema in response
        
    Returns:
        Validated ExecuteQueryParams instance
        
    Raises:
        ValueError: If validation fails
    """
    return ExecuteQueryParams(query=query, params=params, limit=limit, include_schema=include_schema)


def validate_execute_code_params(code: str) -> ExecuteCodeParams:
    """
    Validate execute_code parameters using Pydantic model.
    
    Args:
        code: The 1C code to execute
        
    Returns:
        Validated ExecuteCodeParams instance
        
    Raises:
        ValueError: If validation fails
    """
    return ExecuteCodeParams(code=code)


def validate_get_metadata_params(
    filter: Optional[str] = None,
    meta_type: Optional[Union[str, List[str]]] = None,
    name_mask: Optional[str] = None,
    limit: int = 100,
    sections: Optional[List[str]] = None,
    offset: int = 0,
    extension_name: Optional[str] = None
) -> GetMetadataParams:
    """
    Validate get_metadata parameters using Pydantic model.

    Args:
        filter: Optional metadata filter for detailed structure
        meta_type: Optional metadata type filter for list
        name_mask: Optional name/synonym search mask for list
        limit: Maximum number of objects in list
        sections: Optional list of sections to include in detailed response
        offset: Offset for pagination in list mode
        extension_name: Optional extension name (None=main config, ""=list extensions, "Name"=extension objects)

    Returns:
        Validated GetMetadataParams instance

    Raises:
        ValueError: If validation fails
    """
    return GetMetadataParams(
        filter=filter,
        meta_type=meta_type,
        name_mask=name_mask,
        limit=limit,
        sections=sections,
        offset=offset,
        extension_name=extension_name
    )


def validate_get_event_log_params(
    start_date: Optional[str] = None,
    end_date: Optional[str] = None,
    levels: Optional[List[str]] = None,
    events: Optional[List[str]] = None,
    limit: int = 100,
    object_description: Optional[Dict[str, Any]] = None,
    link: Optional[str] = None,
    data: Optional[str] = None,
    metadata_type: Optional[List[str]] = None,
    user: Optional[List[str]] = None,
    session: Optional[List[int]] = None,
    application: Optional[List[str]] = None,
    computer: Optional[str] = None,
    comment_contains: Optional[str] = None,
    transaction_status: Optional[str] = None,
    same_second_offset: int = 0
) -> GetEventLogParams:
    """
    Validate get_event_log parameters using Pydantic model.
    
    Args:
        start_date: Start date in ISO 8601 format (YYYY-MM-DDTHH:MM:SS)
        end_date: End date in ISO 8601 format
        levels: List of log levels (Information, Warning, Error, Note)
        events: List of event types to filter
        limit: Maximum number of records to return
        object_description: Object description from execute_query results
        link: Navigation link in format e1cib/data/...
        data: Reference to data object (navigation link)
        metadata_type: List of metadata object types (e.g., Документ.РеализацияТоваровУслуг)
        user: List of user names
        session: List of session numbers
        application: List of application types (ThinClient, WebClient, etc.)
        computer: Computer name
        comment_contains: Substring to search in comment
        transaction_status: Transaction status (Committed, RolledBack, etc.)
        
    Returns:
        Validated GetEventLogParams instance
        
    Raises:
        ValueError: If validation fails
    """
    return GetEventLogParams(
        start_date=start_date,
        end_date=end_date,
        levels=levels,
        events=events,
        limit=limit,
        object_description=object_description,
        link=link,
        data=data,
        metadata_type=metadata_type,
        user=user,
        session=session,
        application=application,
        computer=computer,
        comment_contains=comment_contains,
        transaction_status=transaction_status,
        same_second_offset=same_second_offset
    )


def validate_get_object_by_link_params(link: str) -> GetObjectByLinkParams:
    """
    Validate get_object_by_link parameters using Pydantic model.
    
    Validates: Requirements 1.2, 7.1, 7.2, 7.3, 7.4
    
    Args:
        link: Navigation link in format e1cib/data/Type.Name?ref=HexGUID
        
    Returns:
        Validated GetObjectByLinkParams instance
        
    Raises:
        ValueError: If validation fails
    """
    return GetObjectByLinkParams(link=link)


def validate_get_link_of_object_params(
    object_description: Dict[str, Any]
) -> GetLinkOfObjectParams:
    """
    Validate get_link_of_object parameters using Pydantic model.

    Args:
        object_description: Object description from execute_query results

    Returns:
        Validated GetLinkOfObjectParams instance

    Raises:
        ValueError: If validation fails
    """
    return GetLinkOfObjectParams(object_description=object_description)


def validate_find_references_to_object_params(
    target_object_description: Dict[str, Any],
    search_scope: List[str],
    meta_filter: Optional[Dict[str, Any]] = None,
    limit_hits: int = 200,
    limit_per_meta: int = 20,
    timeout_budget_sec: int = 30
) -> FindReferencesToObjectParams:
    """
    Validate find_references_to_object parameters using Pydantic model.

    Args:
        target_object_description: Object description from execute_query results
        search_scope: List of search scopes (documents, catalogs, etc.)
        meta_filter: Optional metadata filter with names and/or name_mask
        limit_hits: Maximum number of hits (default: 200, max: 10000)
        limit_per_meta: Maximum number of hits per metadata object (default: 20, max: 1000)
        timeout_budget_sec: Time budget in seconds (default: 30, min: 5, max: 300)

    Returns:
        Validated FindReferencesToObjectParams instance

    Raises:
        ValueError: If validation fails
    """
    meta_filter_obj = None
    if meta_filter is not None:
        meta_filter_obj = MetaFilter(**meta_filter)

    return FindReferencesToObjectParams(
        target_object_description=target_object_description,
        search_scope=search_scope,
        meta_filter=meta_filter_obj,
        limit_hits=limit_hits,
        limit_per_meta=limit_per_meta,
        timeout_budget_sec=timeout_budget_sec
    )


def validate_get_access_rights_params(
    metadata_object: str,
    user_name: Optional[str] = None,
    rights_filter: Optional[List[str]] = None,
    roles_filter: Optional[List[str]] = None
) -> GetAccessRightsParams:
    """
    Validate get_access_rights parameters using Pydantic model.

    Args:
        metadata_object: Full metadata object name (e.g., "Справочник.Контрагенты")
        user_name: Optional user name for effective rights calculation
        rights_filter: Optional list of rights to show
        roles_filter: Optional list of roles to show

    Returns:
        Validated GetAccessRightsParams instance

    Raises:
        ValueError: If validation fails
    """
    return GetAccessRightsParams(
        metadata_object=metadata_object,
        user_name=user_name,
        rights_filter=rights_filter,
        roles_filter=roles_filter
    )
