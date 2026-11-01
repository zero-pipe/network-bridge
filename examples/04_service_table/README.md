# 04: Service Table

Declare service and method ids with `NB_SERVICE_DEFINE`, install the table, and
look up numeric ids by `service.method`. In a real project this table belongs
in a header shared by the client and the server.

```powershell
.\example_04_service_table.exe
```

Expected output lists the ids of `OrderService.get` and `OrderService.put`.
